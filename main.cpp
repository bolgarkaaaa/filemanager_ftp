#include <iostream>
#include <string>
#include <vector>
#include <curl/curl.h>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <filesystem>
#include <iomanip>
#include <regex>

namespace fs = std::filesystem;

const std::string COLOR_RESET = "\033[0m";
const std::string COLOR_DIR = "\033[1;34m";
const std::string COLOR_FILE = "\033[0m";
const std::string COLOR_SIZE = "\033[0;36m";

std::string format_size_human(uintmax_t size) {
    if (size == 0) return "0 B";
    const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    int i = 0;
    double d_size = static_cast<double>(size);
    while (d_size >= 1024.0 && i < 4) {
        d_size /= 1024.0;
        i++;
    }
    std::stringstream ss;
    ss << std::fixed << std::setprecision((i == 0) ? 0 : 1) << d_size << " " << units[i];
    return ss.str();
}

static size_t write_string_callback(void *contents, size_t size, size_t nmemb, void *userp) {
    ((std::string*)userp)->append((char*)contents, size * nmemb);
    return size * nmemb;
}

struct FtpFile {
    const char *filename;
    FILE *stream;
};

static size_t write_file_callback(void *buffer, size_t size, size_t nmemb, void *stream) {
    struct FtpFile *out = (struct FtpFile *)stream;
    if (!out->stream) {
        out->stream = fopen(out->filename, "wb");
        if (!out->stream) return -1;
    }
    return fwrite(buffer, size, nmemb, out->stream);
}

static size_t read_callback(void *ptr, size_t size, size_t nmemb, void *stream) {
    FILE *file = (FILE *)stream;
    size_t nread = fread(ptr, size, nmemb, file);
    return nread;
}

class LocalFileManager {
private:
    struct FileEntry {
        std::string name;
        bool is_directory;
        uintmax_t size;
    };

    static bool compareFiles(const FileEntry& a, const FileEntry& b) {
        if (a.is_directory != b.is_directory) {
            return a.is_directory > b.is_directory;
        }
        return a.name < b.name;
    }

public:
    void list_directory() {
        try {
            std::vector<FileEntry> entries;
            for (const auto& entry : fs::directory_iterator(fs::current_path())) {
                entries.push_back({
                    entry.path().filename().string(),
                    fs::is_directory(entry.path()),
                    fs::is_regular_file(entry.path()) ? fs::file_size(entry.path()) : 0
                });
            }
            std::sort(entries.begin(), entries.end(), compareFiles);

            std::cout << "\n--- Локальная директория " << fs::current_path() << " ---" << std::endl;
            std::cout << std::left << std::setw(6) << "Тип" 
                      << std::left << std::setw(40) << "Имя" 
                      << std::right << std::setw(15) << "Размер" << std::endl;
            std::cout << std::string(61, '-') << std::endl;

            for (const auto& entry : entries) {
                std::string type_str = entry.is_directory ? "DIR" : "FILE";
                std::string color = entry.is_directory ? COLOR_DIR : COLOR_FILE;

                std::cout << color << std::left << std::setw(6) << type_str
                          << std::left << std::setw(40) << entry.name << COLOR_RESET;
                if (!entry.is_directory) {
                    std::string size_str = format_size_human(entry.size);
                    std::cout << COLOR_SIZE << std::right << std::setw(15) << size_str << COLOR_RESET;
                } else {
                    std::cout << std::right << std::setw(15) << "-";
                }
                std::cout << std::endl;
            }
            std::cout << "-----------------------------------------------------------------" << std::endl;
        } catch (const fs::filesystem_error& e) {
            std::cerr << "Ошибка листинга локальной директории: " << e.what() << std::endl;
        }
    }

    void change_directory(const std::string& path_str) {
        fs::path p(path_str);
        std::error_code ec;
        fs::current_path(p, ec);
        if (ec) {
            std::cerr << "Ошибка смены локальной директории: " << ec.message() << std::endl;
        } else {
            std::cout << "Локальная директория изменена на: " << fs::current_path() << std::endl;
        }
    }

    void create_directory(const std::string& path_str) {
        std::error_code ec;
        if (fs::create_directory(path_str, ec)) {
            std::cout << "Локальная директория '" << path_str << "' создана." << std::endl;
        } else if (ec.value() != 0 && ec.value() != EEXIST) {
            std::cerr << "Ошибка создания локальной директории: " << ec.message() << std::endl;
        } else {
             std::cout << "Локальная директория '" << path_str << "' уже существует или произошла иная ошибка." << std::endl;
        }
    }

    void remove_path(const std::string& path_str) {
        std::error_code ec;
        if (fs::remove(path_str, ec)) {
            std::cout << "Файл/директория '" << path_str << "' удален(а)." << std::endl;
        } else {
            std::cerr << "Ошибка удаления '" << path_str << "': " << ec.message() << std::endl;
        }
    }

    void move_path(const std::string& from_str, const std::string& to_str) {
        std::error_code ec;
        fs::rename(from_str, to_str, ec);
        if (ec) {
            std::cerr << "Ошибка перемещения/переименования: " << ec.message() << std::endl;
        } else {
            std::cout << "Перемещено/переименовано из '" << from_str << "' в '" << to_str << "'" << std::endl;
        }
    }
};


class FtpClient {
private:
    CURL *curl; std::string base_url; std::string user_password;

    std::string ensure_trailing_slash(std::string url) { if (url.back() != '/') url += '/'; return url; }

    struct FtpEntry {
        std::string name;
        bool is_directory;
        uintmax_t size;
    };

    static FtpEntry parse_ftp_entry(const std::string& line) {
        std::regex re("([drwx\\-]+)\\s+\\d+\\s+[^\\s]+\\s+[^\\s]+\\s+([0-9]+)\\s+[^\\s]+\\s+[^\\s]+\\s+(.+)");
        std::smatch matches;
        if (std::regex_match(line, matches, re) && matches.size() == 4) {
            std::string perms = matches[1].str();
            uintmax_t size = std::stoull(matches[2].str());
            std::string name = matches[3].str();
            bool is_dir = (perms.front() == 'd');
            return {name, is_dir, size};
        }
        return {line, false, 0};
    }

    CURLcode perform_curl_operation(const std::string& url, void* write_data_ptr, size_t (*write_func)(void*, size_t, size_t, void*), long upload_mode = 0L, void* read_data_ptr = nullptr, size_t (*read_func)(void*, size_t, size_t, void*) = nullptr) {
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_func);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, write_data_ptr);
        curl_easy_setopt(curl, CURLOPT_UPLOAD, upload_mode);
        if (upload_mode) {
            curl_easy_setopt(curl, CURLOPT_READFUNCTION, read_func);
            curl_easy_setopt(curl, CURLOPT_READDATA, read_data_ptr);
        }
        CURLcode res = curl_easy_perform(curl);
        if (upload_mode) {
            curl_easy_setopt(curl, CURLOPT_UPLOAD, 0L);
        }
        return res;
    }

public:
    FtpClient() : curl(nullptr) {
        curl_global_init(CURL_GLOBAL_DEFAULT);
        curl = curl_easy_init();
        if (!curl) { std::cerr << "Ошибка инициализации libcurl!" << std::endl; exit(1); }
        curl_easy_setopt(curl, CURLOPT_FTP_SKIP_PASV_IP, 1L);
    }

    ~FtpClient() { if (curl) curl_easy_cleanup(curl); curl_global_cleanup(); }

    void connect(const std::string& url, const std::string& userpass) { 
        base_url = ensure_trailing_slash(url);
        user_password = userpass;
        if (!userpass.empty()) { curl_easy_setopt(curl, CURLOPT_USERPWD, userpass.c_str()); }
        std::cout << "Установлен базовый URL: " << base_url << std::endl;
    }

    bool list_directory() {
        std::string list_data;
        
        curl_easy_setopt(curl, CURLOPT_DIRLISTONLY, 0L); 
        
        CURLcode res = perform_curl_operation(base_url, &list_data, write_string_callback);
        
        if (res != CURLE_OK) {
            std::cerr << "Ошибка листинга директории: " << curl_easy_strerror(res) << std::endl;
            return false;
        } else {
            std::cout << "\n--- Содержимое директории " << base_url << " ---" << std::endl;
            std::cout << std::left << std::setw(6) << "Тип" 
                      << std::left << std::setw(40) << "Имя" 
                      << std::right << std::setw(15) << "Размер" << std::endl;
            std::cout << std::string(61, '-') << std::endl;

            std::stringstream ss(list_data);
            std::string line;
            while(std::getline(ss, line, '\n')) {
                if (!line.empty()) {
                    FtpEntry entry = parse_ftp_entry(line);
                    std::string type_str = entry.is_directory ? "DIR" : "FILE";
                    std::string color = entry.is_directory ? COLOR_DIR : COLOR_FILE;

                    std::cout << color << std::left << std::setw(6) << type_str
                              << std::left << std::setw(40) << entry.name << COLOR_RESET;
                    if (!entry.is_directory) {
                        std::string size_str = format_size_human(entry.size);
                        std::cout << COLOR_SIZE << std::right << std::setw(15) << size_str << COLOR_RESET;
                    } else {
                        std::cout << std::right << std::setw(15) << "-";
                    }
                    std::cout << std::endl;
                }
            }
            std::cout << "----------------------------------------------------" << std::endl;
            return true;
        }
    }
    
    void change_directory(const std::string& dir_name) {
        if (dir_name == "..") {
            size_t last_slash = base_url.find_last_of('/', base_url.length() - 2);
            if (last_slash != std::string::npos && last_slash > 5) { 
                base_url = base_url.substr(0, last_slash + 1);
                std::cout << "Директория изменена на: " << base_url << std::endl;
                return;
            } else if (last_slash == std::string::npos) { return; }
        }
        base_url = ensure_trailing_slash(base_url) + dir_name;
        base_url = ensure_trailing_slash(base_url);
        std::cout << "Директория изменена на: " << base_url << std::endl;
    }

    bool download(const std::string& remote_file, const std::string& local_file) {
        std::string full_url = ensure_trailing_slash(base_url) + remote_file;
        struct FtpFile ftpfile = { local_file.c_str(), NULL };
        CURLcode res = perform_curl_operation(full_url, &ftpfile, write_file_callback);
        if (ftpfile.stream) fclose(ftpfile.stream);
        if (res != CURLE_OK) {
            std::cerr << "Ошибка скачивания: " << curl_easy_strerror(res) << std::endl;
            return false;
        }
        std::cout << "Файл '" << remote_file << "' успешно скачан в '" << local_file << "'" << std::endl;
        return res == CURLE_OK;
    }

    bool upload(const std::string& local_file, const std::string& remote_file) {
        FILE *local_stream = fopen(local_file.c_str(), "rb");
        if (!local_stream) {
            std::cerr << "Не удалось открыть локальный файл '" << local_file << "'" << std::endl;
            return false;
        }
        std::string full_url = ensure_trailing_slash(base_url) + remote_file;
        CURLcode res = perform_curl_operation(full_url, nullptr, nullptr, 1L, local_stream, read_callback);
        fclose(local_stream);
        if (res != CURLE_OK) {
            std::cerr << "Ошибка загрузки: " << curl_easy_strerror(res) << std::endl;
            return false;
        }
        std::cout << "Файл '" << local_file << "' успешно загружен как '" << remote_file << "'" << std::endl;
        return res == CURLE_OK;
    }

    bool create_remote_directory(const std::string& dir_name) {
        std::string full_url = ensure_trailing_slash(base_url) + dir_name;
        CURLcode res;
        curl_easy_setopt(curl, CURLOPT_URL, full_url.c_str());
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "MKD");
        std::string response_buffer;
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_string_callback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_buffer);
        res = curl_easy_perform(curl);
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, nullptr);
        if (res != CURLE_OK) {
            std::cerr << "Ошибка создания удаленной директории '" << dir_name << "': " << curl_easy_strerror(res) << std::endl;
            return false;
        }
        std::cout << "Удаленная директория '" << dir_name << "' создана." << std::endl;
        return true;
    }

    bool delete_remote_path(const std::string& path_name, bool is_directory) {
        std::string full_url = ensure_trailing_slash(base_url) + path_name;
        const char* request_type = is_directory ? "RMD" : "DELE";

        CURLcode res;
        curl_easy_setopt(curl, CURLOPT_URL, full_url.c_str());
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, request_type);

        std::string response_buffer;
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_string_callback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_buffer);

        res = curl_easy_perform(curl);
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, nullptr);

        if (res != CURLE_OK) {
            std::cerr << "Ошибка удаления удаленного " << (is_directory ? "директории" : "файла") 
                      << " '" << path_name << "': " << curl_easy_strerror(res) << std::endl;
            return false;
        }
        std::cout << "Удаленный " << (is_directory ? "директория" : "файл") 
                  << " '" << path_name << "' удален(а)." << std::endl;
        return true;
    }

    std::string get_base_url() const {
        return base_url;
    }
};

void display_help() {
    std::cout << "\nДоступные команды (FTP):" << std::endl;
    std::cout << "  connect <url> [user:password] - Подключиться к FTP-серверу (пример: connect ftp://demo.wftpserver.com demo:demo)" << std::endl;
    std::cout << "  ls / dir                      - Листинг удаленной директории (подробный)" << std::endl;
    std::cout << "  cd <directory_name>           - Сменить удаленную директорию" << std::endl;
    std::cout << "  mkdir <directory_name>        - Создать удаленную директорию" << std::endl;
    std::cout << "  rm <name> <is_dir>            - Удалить удаленный файл/директорию (is_dir: 0 или 1)" << std::endl;
    std::cout << "  get <remote_file> <local_file>- Скачать файл" << std::endl;
    std::cout << "  put <local_file> <remote_file>- Загрузить файл" << std::endl;
    std::cout << "Доступные команды (Локальные):" << std::endl;
    std::cout << "  lls / ldir                    - Листинг локальной директории" << std::endl;
    std::cout << "  lcd <directory_name>          - Сменить локальную директорию" << std::endl;
    std::cout << "  lmkdir <directory_name>       - Создать локальную директорию" << std::endl;
    std::cout << "  lrm <path>                    - Удалить локальный файл/директорию" << std::endl;
    std::cout << "  lmv <from> <to>               - Переместить/переименовать локальный файл/директорию" << std::endl;
    std::cout << "Общие команды:" << std::endl;
    std::cout << "  help                          - Показать эту справку" << std::endl;
    std::cout << "  exit                          - Выйти" << std::endl;
}

std::vector<std::string> split_command(const std::string& command_line) {
    std::stringstream ss(command_line);
    std::string item;
    std::vector<std::string> parts;
    while (ss >> item) { parts.push_back(item); }
    return parts;
}

int main() {
    FtpClient ftp_client;
    LocalFileManager local_manager;
    std::string command_line;

    std::cout << "Простой интерактивный FTP-клиент/Файловый менеджер (C++17 required)" << std::endl;
    display_help();

    while (true) {
        std::cout << "\n" << "local:" << fs::current_path().filename().string() 
                  << " | remote:" << fs::path(ftp_client.get_base_url()).filename().string() << "> ";
        
        std::getline(std::cin, command_line);

        std::vector<std::string> args = split_command(command_line);

        if (args.empty()) continue;

        std::string command = args[0]; 
        std::transform(command.begin(), command.end(), command.begin(), ::tolower);

        if (command == "exit") {
            break;
        } else if (command == "help") {
            display_help();
        } else if (command == "connect") {
            if (args.size() >= 2) {
                std::string userpass = (args.size() >= 3) ? args[2] : ""; 
                ftp_client.connect(args[1], userpass); 
            } else { std::cout << "Использование: connect <url> [user:password]" << std::endl; } 
        }
        else if (command == "ls" || command == "dir") { ftp_client.list_directory(); }
        else if (command == "cd") { 
            if (args.size() == 2) { ftp_client.change_directory(args[1]); } else { std::cout << "Использование: cd <directory_name>" << std::endl; } 
        }
        else if (command == "mkdir") { 
            if (args.size() == 2) { ftp_client.create_remote_directory(args[1]); } else { std::cout << "Использование: mkdir <directory_name>" << std::endl; } 
        }
        else if (command == "rm") { 
            if (args.size() == 3) { 
                bool is_dir = (args[2] == "1" || args[2] == "true");
                ftp_client.delete_remote_path(args[1], is_dir); 
            } else { std::cout << "Использование: rm <name> <is_dir(0|1)>" << std::endl; } 
        }
        else if (command == "get") { 
            if (args.size() == 3) { ftp_client.download(args[1], args[2]); } else { std::cout << "Использование: get <remote_file> <local_file>" << std::endl; } 
        }
        else if (command == "put") { 
            if (args.size() == 3) { ftp_client.upload(args[1], args[2]); } else { std::cout << "Использование: put <local_file> <remote_file>" << std::endl; } 
        }
        else if (command == "lls" || command == "ldir") { local_manager.list_directory(); }
        else if (command == "lcd") { 
            if (args.size() == 2) { local_manager.change_directory(args[1]); } else { std::cout << "Использование: lcd <directory_name>" << std::endl; } 
        }
        else if (command == "lmkdir") { 
            if (args.size() == 2) { local_manager.create_directory(args[1]); } else { std::cout << "Использование: lmkdir <directory_name>" << std::endl; } 
        }
        else if (command == "lrm") { 
            if (args.size() == 2) { local_manager.remove_path(args[1]); } else { std::cout << "Использование: lrm <path>" << std::endl; } 
        }
        else if (command == "lmv") { 
            if (args.size() == 3) { local_manager.move_path(args[1], args[2]); } else { std::cout << "Использование: lmv <from_path> <to_path>" << std::endl; } 
        }
        else { std::cout << "Неизвестная команда. Введите 'help' для списка команд." << std::endl; }
    }

    return 0;
}
