#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace fs = std::filesystem;

struct Document {
    int id;
    std::string path;
    std::string title;
    std::string text;
    int token_count = 0;
};

struct SearchResult {
    int doc_id;
    double score;
};

static const std::unordered_set<std::string> STOPWORDS = {
    "the", "and", "for", "with", "that", "this", "from", "are", "was", "were",
    "you", "your", "have", "has", "had", "not", "but", "can", "will", "would",
    "should", "could", "into", "over", "under", "there", "their", "they",
    "them", "then", "than", "also", "about", "what", "when", "where", "which",
    "who", "why", "how", "our", "out", "its", "all", "one", "two", "more",
    "less", "use", "used", "using", "such", "may", "these", "those", "been",
    "being", "because", "between", "within", "without", "each", "other",
    "paper", "section", "figure", "table"
};

std::string shell_quote(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') {
            out += "'\\''";
        } else {
            out += c;
        }
    }
    out += "'";
    return out;
}

std::optional<std::string> run_command(const std::string& command) {
    std::array<char, 4096> buffer{};
    std::string result;

    FILE* pipe = popen(command.c_str(), "r");
    if (!pipe) {
        return std::nullopt;
    }

    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
        result += buffer.data();
    }

    int exit_code = pclose(pipe);
    if (exit_code != 0 && result.empty()) {
        return std::nullopt;
    }

    return result;
}

bool command_exists(const std::string& command) {
    std::string check = "command -v " + command + " >/dev/null 2>&1";
    return std::system(check.c_str()) == 0;
}

std::string lowercase_copy(const std::string& s) {
    std::string out;
    out.reserve(s.size());

    for (unsigned char c : s) {
        out.push_back(static_cast<char>(std::tolower(c)));
    }

    return out;
}

std::vector<std::string> tokenize(const std::string& text, bool remove_stopwords = true) {
    std::vector<std::string> tokens;
    std::string current;

    auto flush = [&]() {
        if (current.size() >= 2) {
            if (!remove_stopwords || STOPWORDS.find(current) == STOPWORDS.end()) {
                tokens.push_back(current);
            }
        }
        current.clear();
    };

    for (unsigned char c : text) {
        if (std::isalnum(c)) {
            current.push_back(static_cast<char>(std::tolower(c)));
        } else {
            flush();
        }
    }

    flush();
    return tokens;
}

std::string extract_pdf_text(const fs::path& pdf_path) {
    std::string command =
        "pdftotext -layout -enc UTF-8 " +
        shell_quote(pdf_path.string()) +
        " - 2>/dev/null";

    auto output = run_command(command);
    if (!output.has_value()) {
        return "";
    }

    return output.value();
}

std::string clean_snippet(std::string s) {
    for (char& c : s) {
        if (c == '\n' || c == '\r' || c == '\t') {
            c = ' ';
        }
    }

    while (s.find("  ") != std::string::npos) {
        s.erase(s.find("  "), 1);
    }

    return s;
}

std::string make_snippet(const std::string& text, const std::vector<std::string>& query_terms) {
    if (text.empty()) {
        return "";
    }

    std::string lower = lowercase_copy(text);
    size_t best_pos = std::string::npos;

    for (const auto& term : query_terms) {
        size_t pos = lower.find(term);
        if (pos != std::string::npos) {
            if (best_pos == std::string::npos || pos < best_pos) {
                best_pos = pos;
            }
        }
    }

    if (best_pos == std::string::npos) {
        best_pos = 0;
    }

    const size_t window = 260;
    size_t start = best_pos > 80 ? best_pos - 80 : 0;
    size_t len = std::min(window, text.size() - start);

    std::string snippet = text.substr(start, len);
    snippet = clean_snippet(snippet);

    if (start > 0) {
        snippet = "... " + snippet;
    }

    if (start + len < text.size()) {
        snippet += " ...";
    }

    return snippet;
}

class PdfSearchEngine {
public:
    void index_directory(const fs::path& root) {
        int indexed = 0;
        int skipped = 0;

        for (const auto& entry : fs::recursive_directory_iterator(root)) {
            if (!entry.is_regular_file()) {
                continue;
            }

            fs::path path = entry.path();
            if (lowercase_copy(path.extension().string()) != ".pdf") {
                continue;
            }

            std::cout << "Indexing: " << path.string() << "\n";
            std::string text = extract_pdf_text(path);

            if (text.empty()) {
                std::cerr << "  Skipped, no extractable text.\n";
                skipped++;
                continue;
            }

            add_document(path, text);
            indexed++;
        }

        std::cout << "\nIndexed " << indexed << " PDFs";
        if (skipped > 0) {
            std::cout << " and skipped " << skipped;
        }
        std::cout << ".\n\n";
    }

    std::vector<SearchResult> search(const std::string& query, bool require_all_terms, int limit = 10) const {
        std::vector<std::string> query_terms = tokenize(query);

        if (query_terms.empty()) {
            return {};
        }

        std::sort(query_terms.begin(), query_terms.end());
        query_terms.erase(std::unique(query_terms.begin(), query_terms.end()), query_terms.end());

        std::vector<double> scores(docs.size(), 0.0);
        std::vector<int> matched_terms(docs.size(), 0);

        for (const auto& term : query_terms) {
            auto posting_it = inverted_index.find(term);
            if (posting_it == inverted_index.end()) {
                if (require_all_terms) {
                    return {};
                }
                continue;
            }

            const auto& posting = posting_it->second;
            double df = static_cast<double>(posting.size());
            double idf = std::log((static_cast<double>(docs.size()) + 1.0) / (df + 1.0)) + 1.0;

            for (const auto& [doc_id, freq] : posting) {
                double tf = std::log(1.0 + static_cast<double>(freq));
                scores[doc_id] += tf * idf;
                matched_terms[doc_id]++;
            }
        }

        std::vector<SearchResult> results;

        for (size_t i = 0; i < docs.size(); i++) {
            bool valid = require_all_terms
                ? matched_terms[i] == static_cast<int>(query_terms.size())
                : matched_terms[i] > 0;

            if (valid && scores[i] > 0.0) {
                results.push_back({static_cast<int>(i), scores[i]});
            }
        }

        std::sort(results.begin(), results.end(), [](const SearchResult& a, const SearchResult& b) {
            return a.score > b.score;
        });

        if (static_cast<int>(results.size()) > limit) {
            results.resize(limit);
        }

        return results;
    }

    const Document& get_document(int doc_id) const {
        return docs.at(doc_id);
    }

    int document_count() const {
        return static_cast<int>(docs.size());
    }

    int vocabulary_size() const {
        return static_cast<int>(inverted_index.size());
    }

private:
    std::vector<Document> docs;

    // term -> doc_id -> frequency
    std::unordered_map<std::string, std::unordered_map<int, int>> inverted_index;

    void add_document(const fs::path& path, const std::string& text) {
        int id = static_cast<int>(docs.size());
        std::vector<std::string> tokens = tokenize(text);

        Document doc;
        doc.id = id;
        doc.path = path.string();
        doc.title = path.filename().string();
        doc.text = text;
        doc.token_count = static_cast<int>(tokens.size());

        docs.push_back(std::move(doc));

        for (const auto& token : tokens) {
            inverted_index[token][id]++;
        }
    }
};

std::string trim(const std::string& s) {
    size_t start = 0;
    while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start]))) {
        start++;
    }

    size_t end = s.size();
    while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1]))) {
        end--;
    }

    return s.substr(start, end - start);
}

void print_help() {
    std::cout << "Commands:\n";
    std::cout << "  keyword search        Search PDFs requiring all keywords by default\n";
    std::cout << "  or: keyword search    Search PDFs matching any keyword\n";
    std::cout << "  :stats                Show index stats\n";
    std::cout << "  :help                 Show this help\n";
    std::cout << "  :quit                 Exit\n\n";
}

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "Usage: " << argv[0] << " /path/to/pdf/folder\n";
        return 1;
    }

    if (!command_exists("pdftotext")) {
        std::cerr << "Error: pdftotext not found.\n";
        std::cerr << "Install Poppler first.\n";
        std::cerr << "macOS: brew install poppler\n";
        std::cerr << "Ubuntu/Debian: sudo apt install poppler-utils\n";
        return 1;
    }

    fs::path root = argv[1];
    if (!fs::exists(root) || !fs::is_directory(root)) {
        std::cerr << "Error: path is not a directory: " << root.string() << "\n";
        return 1;
    }

    PdfSearchEngine engine;
    engine.index_directory(root);

    if (engine.document_count() == 0) {
        std::cerr << "No searchable PDFs found.\n";
        return 1;
    }

    print_help();

    std::string line;
    while (true) {
        std::cout << "search> ";
        if (!std::getline(std::cin, line)) {
            break;
        }

        line = trim(line);
        if (line.empty()) {
            continue;
        }

        if (line == ":quit" || line == ":q" || line == "exit") {
            break;
        }

        if (line == ":help") {
            print_help();
            continue;
        }

        if (line == ":stats") {
            std::cout << "Documents: " << engine.document_count() << "\n";
            std::cout << "Vocabulary terms: " << engine.vocabulary_size() << "\n\n";
            continue;
        }

        bool require_all_terms = true;
        std::string query = line;

        if (query.rfind("or:", 0) == 0) {
            require_all_terms = false;
            query = trim(query.substr(3));
        }

        std::vector<std::string> query_terms = tokenize(query);
        auto results = engine.search(query, require_all_terms, 10);

        if (results.empty()) {
            std::cout << "No results. Try `or: " << query << "` for looser matching.\n\n";
            continue;
        }

        for (size_t i = 0; i < results.size(); i++) {
            const auto& result = results[i];
            const auto& doc = engine.get_document(result.doc_id);

            std::cout << "[" << i + 1 << "] " << doc.title << "\n";
            std::cout << "    Score: " << result.score << "\n";
            std::cout << "    Path:  " << doc.path << "\n";
            std::cout << "    " << make_snippet(doc.text, query_terms) << "\n\n";
        }
    }

    std::cout << "bye!\n";
    return 0;
}
