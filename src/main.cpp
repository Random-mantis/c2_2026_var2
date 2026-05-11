#include <algorithm>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace fs = std::filesystem;

struct Error : std::runtime_error { using std::runtime_error::runtime_error; };

static std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace((unsigned char)s[b])) ++b;
    while (e > b && std::isspace((unsigned char)s[e - 1])) --e;
    return s.substr(b, e - b);
} // Избавление от пробелов

static std::string upper(std::string s) {
    for (char& c : s) c = (char)std::toupper((unsigned char)c);
    return s;
} //Превращение в верхний регистр.

static bool ieq(const std::string& a, const std::string& b) { 
    return upper(a) == upper(b); 
} //Проверка на эквивалентность без учёта регистра

static std::vector<std::string> splitTop(const std::string& s, char sep) {
    std::vector<std::string> out;
    int p = 0;
    bool q = false;
    std::string cur;
    for (char c : s) {
        if (c == '"') q = !q;
        if (!q && c == '(') ++p;
        if (!q && c == ')') --p;
        if (!q && p == 0 && c == sep) {
            out.push_back(trim(cur));
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    if (!trim(cur).empty()) out.push_back(trim(cur));
    return out;
} // Разбивает строку по разделителю (можно в sep поставить " " по умолчанию.)

static std::string nowStamp() {
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
    std::time_t tt = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &tt);
#else
    localtime_r(&tt, &tm);
#endif
    std::ostringstream os;
    os << std::put_time(&tm, "%Y.%m.%d-%H:%M:%S") << "." << std::setw(3) << std::setfill('0') << ms.count();
    return os.str();
} //Ввод текущего времени в поток ввода

static bool validName(const std::string& s) {
    if (s.empty() || std::isdigit((unsigned char)s[0])) return false;
    for (char c : s) if (!std::isalnum((unsigned char)c) && c != '_') return false;
    return true;
} // Проверка на правильность имён переменных.

struct InternPool {
    std::set<std::string> values;
    const std::string& get(const std::string& s) { 
        return *values.insert(s).first; 
    } //Возвращает позицию (итератор) на переменную.
}; // Структура для возможности паралелльных процессов.

struct Value {
    enum Type { Null, Int, String } type = Null;
    int i = 0;
    const std::string* s = nullptr;

    static Value null() { 
        return {}; 
    }
    
    static Value integer(int v) { 
        Value x; 
        x.type = Int; 
        x.i = v; 
        return x; 
    }

    static Value string(const std::string& v, InternPool& pool) { 
        Value x; 
        x.type = String; 
        x.s = &pool.get(v); 
        return x; 
    } //Возаращение value разных типов

    std::string str() const {
        if (type == Null) return "NULL";
        if (type == Int) return std::to_string(i);
        return *s;
    } //Возвращение значения переменной в строчном виде.
    
    std::string json() const {
        if (type == Null) return "null";
        if (type == Int) return std::to_string(i);
        std::string r = "\"";
        for (char c : *s) { 
            if (c == '"' || c == '\\') r.push_back('\\'); 
            r.push_back(c); 
        }
        return r + "\"";
    } //Возвращение value для json файла
}; // Структура для значений в базе данных

static int cmpValue(const Value& a, const Value& b) {
    if (a.type != b.type) return (int)a.type < (int)b.type ? -1 : 1;
    if (a.type == Value::Null) return 0;
    if (a.type == Value::Int) return (a.i < b.i) ? -1 : (a.i > b.i ? 1 : 0);
    return a.s->compare(*b.s);
} //Сравнение значений переменных

static bool operator<(const Value& a, const Value& b) { 
    return cmpValue(a, b) < 0; 
} 
static bool operator==(const Value& a, const Value& b) { 
    return cmpValue(a, b) == 0; 
} //Переопределённые операторы сравнения

enum class ColType { Int, String };
struct Column {
    std::string name;
    ColType type;
    bool notNull = false;
    bool indexed = false;
    bool hasDefault = false;
    Value def;
}; //Структура для столбика в базе данных.

using Row = std::vector<Value>; //Тип строк в базе данных

template<class K, class V, size_t ORDER = 4>
class BPlusTree {
    struct Node {
        bool leaf; //Является ли узел листом
        std::vector<K> keys; //Массив ключей
        std::vector<std::unique_ptr<Node>> child; // Указатель на ребёнка
        std::vector<V> vals; //Массив значений
        Node* next = nullptr; //Следующий элемент в односвязном списке листьев
        explicit Node(bool l) : leaf(l) {} //Конструктор для определения типа узла
    }; // Структура узла дерева
    std::unique_ptr<Node> root = std::make_unique<Node>(true); //Корень дерева.

    void splitChild(Node* parent, size_t idx) {
        Node* n = parent->child[idx].get();
        auto right = std::make_unique<Node>(n->leaf);
        size_t mid = n->keys.size() / 2;
        if (n->leaf) {
            right->keys.assign(n->keys.begin() + mid, n->keys.end());
            right->vals.assign(n->vals.begin() + mid, n->vals.end());
            n->keys.resize(mid);
            n->vals.resize(mid);
            right->next = n->next;
            n->next = right.get();
            parent->keys.insert(parent->keys.begin() + idx, right->keys.front());
        } else {
            K up = n->keys[mid];
            right->keys.assign(n->keys.begin() + mid + 1, n->keys.end());
            std::move(n->child.begin() + mid + 1, n->child.end(), std::back_inserter(right->child));
            n->keys.resize(mid);
            n->child.resize(mid + 1);
            parent->keys.insert(parent->keys.begin() + idx, up);
        }
        parent->child.insert(parent->child.begin() + idx + 1, std::move(right));
    } //Разделение узла дерева для вставки нового ключа при переполнении. Должна делить не на 2 а как при быстрой сортировке


    bool insertNonFull(Node* n, const K& k, const V& v) {
        if (n->leaf) {
            auto it = std::lower_bound(n->keys.begin(), n->keys.end(), k);
            size_t pos = (size_t)(it - n->keys.begin());
            if (it != n->keys.end() && !(*it < k) && !(k < *it)) return false;
            n->keys.insert(it, k);
            n->vals.insert(n->vals.begin() + pos, v);
            return true;
        }
        size_t i = (size_t)(std::upper_bound(n->keys.begin(), n->keys.end(), k) - n->keys.begin());
        if (n->child[i]->keys.size() >= ORDER) {
            splitChild(n, i);
            if (!(k < n->keys[i])) ++i;
        }
        return insertNonFull(n->child[i].get(), k, v);
    } //Вставка ключа в узел, если он не переполнен

public:
    bool insert(const K& k, const V& v) {
        if (root->keys.size() >= ORDER) {
            auto nr = std::make_unique<Node>(false);
            nr->child.push_back(std::move(root));
            root = std::move(nr);
            splitChild(root.get(), 0);
        }
        return insertNonFull(root.get(), k, v); 
    }// Вставка в дерево

    bool find(const K& k, V& v) const {
        Node* n = root.get();
        while (!n->leaf) n = n->child[std::upper_bound(n->keys.begin(), n->keys.end(), k) - n->keys.begin()].get();
        auto it = std::lower_bound(n->keys.begin(), n->keys.end(), k);
        if (it == n->keys.end() || *it < k || k < *it) return false;
        v = n->vals[it - n->keys.begin()];
        return true;
    } //Проверка нахождения ключа в дереве, Нарушение SOLID!!!
}; //B+ дерево, а где функция удаления?

class Table {
    fs::path dir; // Путь к базе данных.
    InternPool& pool; // Структура данных для паралелльности
public:
    std::vector<Column> cols; //Столбики базы данных
    std::vector<Row> rows; //Строки базы данных
    std::unordered_map<std::string, BPlusTree<Value, size_t>> indexes; //Для нахождения нужных элемнтов по имени столбца

    Table(fs::path d, InternPool& p) : dir(std::move(d)), pool(p) {} //Конструктор таблицы

    int colId(const std::string& name) const {
        for (size_t i = 0; i < cols.size(); ++i) if (cols[i].name == name) return (int)i;
        throw Error("unknown column: " + name);
    } //Вернуть индекс стобца с именем name в случае отсутствия -- ошибка

    fs::path schemaFile() const { 
        return dir / "schema.txt"; 
    } // Возвращает путь для файла со схемой базы данных

    fs::path rowsFile() const { 
        return dir / "rows.tsv"; 
    } // Возвращает путь для самой базы данных

    fs::path histFile() const { 
        return dir / "history.log"; 
    } // Возвращает путь к файлу с историей изменения базы данныз.

    Value parseValue(const std::string& raw, ColType type) {
        std::string x = trim(raw);
        if (ieq(x, "NULL")) return Value::null();
        if (type == ColType::Int) {
            try { 
                return Value::integer(std::stoi(x)); 
            } catch (...) { 
                throw Error("expected int value: " + x); 
            }
        }
        if (x.size() < 2 || x.front() != '"' || x.back() != '"') throw Error("expected string literal: " + x);
        return Value::string(x.substr(1, x.size() - 2), pool);
    } // Получаем значение

    void validateRow(const Row& r, int ignore = -1) const {
        for (size_t c = 0; c < cols.size(); ++c) {
            if ((cols[c].notNull || cols[c].indexed) && r[c].type == Value::Null)
                throw Error("column cannot be NULL: " + cols[c].name);
            if (r[c].type != Value::Null && ((cols[c].type == ColType::Int) != (r[c].type == Value::Int)))
                throw Error("type mismatch in column: " + cols[c].name);
            if (cols[c].indexed) {
                for (size_t i = 0; i < rows.size(); ++i) {
                    if ((int)i != ignore && rows[i][c] == r[c]) throw Error("INDEXED column is not unique: " + cols[c].name);
                }
            }
        }
    } // Проверка строк и столбцов на валиндность

    void rebuildIndexes() {
        indexes.clear();
        for (const auto& c : cols) if (c.indexed) indexes.emplace(c.name, BPlusTree<Value, size_t>{});
        for (size_t r = 0; r < rows.size(); ++r)
            for (size_t c = 0; c < cols.size(); ++c)
                if (cols[c].indexed) indexes[cols[c].name].insert(rows[r][c], r);
    } // Обновление индексов

    void load() {
        cols.clear(); rows.clear();
        std::ifstream s(schemaFile());
        std::string line;
        while (std::getline(s, line)) {
            if (trim(line).empty()) continue;
            auto p = splitTop(line, '|');
            Column c;
            c.name = p.at(0);
            c.type = ieq(p.at(1), "int") ? ColType::Int : ColType::String;
            c.notNull = p.at(2) == "1";
            c.indexed = p.at(3) == "1";
            c.hasDefault = p.at(4) == "1";
            if (c.hasDefault) c.def = parseValue(p.at(5), c.type);
            cols.push_back(c);
        }
        std::ifstream r(rowsFile());
        while (std::getline(r, line)) {
            auto parts = splitTop(line, '\t');
            Row row;
            for (size_t i = 0; i < cols.size(); ++i) row.push_back(parseValue(i < parts.size() ? parts[i] : "NULL", cols[i].type));
            rows.push_back(row);
        }
        rebuildIndexes();
    } // Загружаем базу данных из файла.

    void save() const {
        fs::create_directories(dir);
        std::ofstream s(schemaFile(), std::ios::trunc);
        for (const auto& c : cols) {
            s << c.name << '|' << (c.type == ColType::Int ? "int" : "string") << '|'
              << c.notNull << '|' << c.indexed << '|' << c.hasDefault << '|';
            if (c.hasDefault) s << (c.def.type == Value::String ? "\"" + c.def.str() + "\"" : c.def.str());
            s << '\n';
        }
        std::ofstream r(rowsFile(), std::ios::trunc);
        for (const auto& row : rows) {
            for (size_t i = 0; i < row.size(); ++i) {
                if (i) r << '\t';
                if (row[i].type == Value::String) r << '"' << row[i].str() << '"'; else r << row[i].str();
            }
            r << '\n';
        }
    } // Сохраняем изменения в базе данных

    void snapshot() const {
        fs::create_directories(dir);
        std::ofstream h(histFile(), std::ios::app);
        h << "@" << nowStamp() << "\n";
        for (const auto& row : rows) {
            for (size_t i = 0; i < row.size(); ++i) {
                if (i) h << '\t';
                if (row[i].type == Value::String) h << '"' << row[i].str() << '"'; else h << row[i].str();
            }
            h << '\n';
        }
        h << "#\n";
    } //Создание копии состояния базы данных

    void revert(const std::string& ts) {
        std::ifstream h(histFile());
        if (!h) throw Error("history is empty");
        std::string line, curTs;
        std::vector<std::string> cur, best;
        while (std::getline(h, line)) {
            if (!line.empty() && line[0] == '@') { curTs = line.substr(1); cur.clear(); }
            else if (line == "#") { if (curTs <= ts) best = cur; }
            else cur.push_back(line);
        }
        if (best.empty()) throw Error("no snapshot before timestamp");
        rows.clear();
        for (auto& l : best) {
            auto parts = splitTop(l, '\t');
            Row row;
            for (size_t i = 0; i < cols.size(); ++i) row.push_back(parseValue(i < parts.size() ? parts[i] : "NULL", cols[i].type));
            rows.push_back(row);
        }
        rebuildIndexes();
        save();
    } //Возврат к последней сделанной копии базы данных.
};

struct Lexer {
    std::vector<std::string> t;
    size_t p = 0;
    explicit Lexer(const std::string& s) {
        for (size_t i = 0; i < s.size();) {
            if (std::isspace((unsigned char)s[i])) { 
                ++i; 
                continue; 
            }
            
            if (s[i] == '"') {
                size_t j = i + 1;
                while (j < s.size() && s[j] != '"') { 
                    if (s[j] == '\\') ++j; ++j; 
                }
                
                if (j >= s.size()) throw Error("unterminated string literal");
                t.push_back(s.substr(i, j - i + 1)); 
                i = j + 1; 
                continue;
            }

            if (std::ispunct((unsigned char)s[i]) && s[i] != '_' && s[i] != '.') {
                if (i + 1 < s.size() && (s.substr(i, 2) == "==" || s.substr(i, 2) == "!=" || s.substr(i, 2) == "<=" || s.substr(i, 2) == ">=")) {
                    t.push_back(s.substr(i, 2)); 
                    i += 2;
                } else t.push_back(std::string(1, s[i++]));
                continue;
            }
            size_t j = i;
            while (j < s.size() && (std::isalnum((unsigned char)s[j]) || s[j] == '_' || s[j] == '.')) ++j;
            t.push_back(s.substr(i, j - i)); i = j;
        }
    } //Функция, анализирующая строку на правильность.
    bool has() const { 
        return p < t.size(); 
    }//Функция, проверяющая все ли строки использованы.

    std::string peek() const { 
        return has() ? t[p] : ""; 
    } //Посмотреть текущую строку

    std::string get() { 
        if (!has()) throw Error("unexpected end of command"); 
        return t[p++]; 
    }//Получить текущую строку и двигаемся дальше, Нарушение SOLID

    bool eat(const std::string& x) { 
        if (has() && ieq(peek(), x)) { 
            ++p; 
            return true; 
        } 
        return false; 
    } //Проверка использована ли строка
    
    void need(const std::string& x) { 
        if (!eat(x)) throw Error("expected: " + x); 
    } // Проверка на то, что строка нужна
}; // Структура для анализа команд для Базы Данных.

class Engine {
    fs::path root = "data"; //Корень баз данных
    std::string currentDb; //Имя текущей базы данных.
    InternPool pool; 

    fs::path dbPath(const std::string& db) const { 
        return root / db; 
    } //Вернуть путь к базе данных с именем db

    std::pair<std::string,std::string> resolveTable(std::string name) const {
        auto dot = name.find('.');
        if (dot != std::string::npos) return {name.substr(0, dot), name.substr(dot + 1)};
        if (currentDb.empty()) throw Error("database is not selected; use USE database_name");
        return {currentDb, name};
    } // Проверка разрешения базы данных

    Table openTable(const std::string& name) {
        auto [db, tb] = resolveTable(name);
        Table t(dbPath(db) / tb, pool);
        if (!fs::exists(t.schemaFile())) throw Error("table does not exist: " + name);
        t.load();
        return t;
    }// Получение базы данных с именем name.

    void log(const std::string& q, const std::string& status) {
        fs::create_directories(root);
        std::ofstream f(root / "access.log", std::ios::app);
        f << nowStamp() << " client=local handler=cli status=" << status << " query=" << q << "\n";
    }// Вывод состояния в поток вывода.

    Value literalOrColumn(Table& t, const Row& r, const std::string& tok) {
        if (!tok.empty() && tok.front() == '"') return Value::string(tok.substr(1, tok.size() - 2), pool);
        if (ieq(tok, "NULL")) return Value::null();
        if (!tok.empty() && (std::isdigit((unsigned char)tok[0]) || tok[0] == '-')) return Value::integer(std::stoi(tok));
        return r[t.colId(tok)];
    } //Функция для различий между столбцом и КОМАНДАМИ ???

    bool predicate(Table& t, const Row& r, Lexer& lx) {
        if (lx.eat("(")) { bool v = expr(t, r, lx); lx.need(")"); return v; }
        std::string left = lx.get();
        if (lx.eat("BETWEEN")) {
            Value a = literalOrColumn(t, r, left), lo = literalOrColumn(t, r, lx.get());
            lx.need("AND");
            Value hi = literalOrColumn(t, r, lx.get());
            return cmpValue(a, lo) >= 0 && cmpValue(a, hi) < 0;
        }
        if (lx.eat("LIKE")) {
            Value a = literalOrColumn(t, r, left), pat = literalOrColumn(t, r, lx.get());
            if (a.type != Value::String || pat.type != Value::String) throw Error("LIKE requires strings");
            return std::regex_match(*a.s, std::regex(*pat.s));
        }
        std::string op = lx.get();
        Value a = literalOrColumn(t, r, left), b = literalOrColumn(t, r, lx.get());
        int c = cmpValue(a, b);
        if (op == "==") return c == 0; if (op == "!=") return c != 0; if (op == "<") return c < 0;
        if (op == ">") return c > 0; if (op == "<=") return c <= 0; if (op == ">=") return c >= 0;
        throw Error("unknown comparison operator: " + op);
    } // Функция для определения операций сравнений

    bool andExpr(Table& t, const Row& r, Lexer& lx) {
        bool v = predicate(t, r, lx);
        while (lx.eat("AND")) v = predicate(t, r, lx) && v;
        return v;
    } // Для определения оператора and

    bool expr(Table& t, const Row& r, Lexer& lx) {
        bool v = andExpr(t, r, lx);
        while (lx.eat("OR")) v = andExpr(t, r, lx) || v;
        return v;
    } // Для определения оператора щк

    std::vector<size_t> filter(Table& t, const std::string& where) {
        std::vector<size_t> ids;
        if (trim(where).empty()) { for (size_t i = 0; i < t.rows.size(); ++i) ids.push_back(i); return ids; }
        Lexer quick(where);
        if (quick.t.size() == 3 && quick.t[1] == "==" && t.indexes.count(quick.t[0])) {
            int c = t.colId(quick.t[0]); Value k = t.parseValue(quick.t[2], t.cols[c].type); size_t id = 0;
            if (t.indexes[quick.t[0]].find(k, id)) ids.push_back(id);
            return ids;
        }
        for (size_t i = 0; i < t.rows.size(); ++i) {
            Lexer lx(where);
            if (expr(t, t.rows[i], lx)) ids.push_back(i);
        }
        return ids;
    } // Для отбора данных из таблицы.

    std::string jsonRows(Table& t, const std::vector<size_t>& ids, const std::vector<std::pair<std::string,std::string>>& cols) {
        std::ostringstream o; o << "[";
        for (size_t k = 0; k < ids.size(); ++k) {
            if (k) o << ",";
            o << "{";
            for (size_t i = 0; i < cols.size(); ++i) {
                if (i) o << ",";
                int c = t.colId(cols[i].first);
                o << "\"" << cols[i].second << "\":" << t.rows[ids[k]][c].json();
            }
            o << "}";
        }
        o << "]";
        return o.str();
    } // Получение данных в json формате

public:
    std::string exec(const std::string& q0) {
        std::string q = trim(q0);
        if (!q.empty() && q.back() == ';') q.pop_back();
        try {
            std::string r = run(q);
            log(q, "OK");
            return r;
        } catch (const std::exception& e) {
            log(q, std::string("ERROR:") + e.what());
            return std::string("ERROR: ") + e.what();
        }
    } // Выполнение команды

    std::string run(const std::string& q) {
        Lexer lx(q);
        if (!lx.has()) return "";
        std::string cmd = upper(lx.get());
        if (cmd == "CREATE" && lx.eat("DATABASE")) {
            std::string db = lx.get(); if (!validName(db)) throw Error("invalid database name");
            fs::create_directories(dbPath(db)); return "OK";
        }
        if (cmd == "DROP" && lx.eat("DATABASE")) {
            fs::remove_all(dbPath(lx.get())); if (currentDb == lx.peek()) currentDb.clear(); return "OK";
        }
        if (cmd == "USE") {
            std::string db = lx.get(); if (!fs::exists(dbPath(db))) throw Error("database does not exist: " + db);
            currentDb = db; return "OK";
        }
        if (cmd == "CREATE" && lx.eat("TABLE")) {
            if (currentDb.empty()) throw Error("database is not selected");
            std::string name = lx.get(); if (!validName(name)) throw Error("invalid table name");
            Table t(dbPath(currentDb) / name, pool);
            if (fs::exists(t.schemaFile())) throw Error("table already exists: " + name);
            lx.need("(");
            while (!lx.eat(")")) {
                Column c; c.name = lx.get(); if (!validName(c.name)) throw Error("invalid column name");
                std::string typ = lx.get(); c.type = ieq(typ, "int") ? ColType::Int : (ieq(typ, "string") ? ColType::String : throw Error("unknown type: " + typ));
                while (!lx.eat(",") && lx.peek() != ")") {
                    if (lx.eat("NOT_NULL")) c.notNull = true;
                    else if (lx.eat("INDEXED")) { c.indexed = true; c.notNull = true; }
                    else if (lx.eat("DEFAULT")) { c.hasDefault = true; c.def = t.parseValue(lx.get(), c.type); }
                    else throw Error("unknown column modifier: " + lx.peek());
                }
                t.cols.push_back(c);
            }
            t.save(); return "OK";
        }
        if (cmd == "DROP" && lx.eat("TABLE")) {
            auto [db, tb] = resolveTable(lx.get());
            fs::remove_all(dbPath(db) / tb); return "OK";
        }
        if (cmd == "INSERT") {
            lx.need("INTO"); std::string name = lx.get(); Table t = openTable(name);
            lx.need("("); std::vector<std::string> names; while (!lx.eat(")")) { names.push_back(lx.get()); lx.eat(","); }
            lx.need("VALUE");
            do {
                lx.need("("); Row row(t.cols.size(), Value::null());
                for (size_t i = 0; i < t.cols.size(); ++i) if (t.cols[i].hasDefault) row[i] = t.cols[i].def;
                for (size_t i = 0; i < names.size(); ++i) {
                    int c = t.colId(names[i]); row[c] = t.parseValue(lx.get(), t.cols[c].type); lx.eat(",");
                }
                lx.need(")");
                t.validateRow(row); t.rows.push_back(row);
            } while (lx.eat(","));
            t.rebuildIndexes(); t.save(); return "OK";
        }
        if (cmd == "UPDATE") {
            std::string name = lx.get(); Table t = openTable(name); t.snapshot();
            lx.need("SET"); std::vector<std::pair<int,Value>> set;
            while (!lx.eat("WHERE")) {
                std::string c = lx.get(); lx.need("="); int id = t.colId(c); set.push_back({id, t.parseValue(lx.get(), t.cols[id].type)}); lx.eat(",");
            }
            std::string where; while (lx.has()) where += (where.empty() ? "" : " ") + lx.get();
            for (size_t id : filter(t, where)) { Row nr = t.rows[id]; for (auto& p : set) nr[p.first] = p.second; t.validateRow(nr, (int)id); t.rows[id] = nr; }
            t.rebuildIndexes(); t.save(); return "OK";
        }
        if (cmd == "DELETE") {
            lx.need("FROM"); std::string name = lx.get(); Table t = openTable(name); t.snapshot();
            lx.need("WHERE"); std::string where; while (lx.has()) where += (where.empty() ? "" : " ") + lx.get();
            auto ids = filter(t, where); std::set<size_t> del(ids.begin(), ids.end()); std::vector<Row> keep;
            for (size_t i = 0; i < t.rows.size(); ++i) if (!del.count(i)) keep.push_back(t.rows[i]);
            t.rows = keep; t.rebuildIndexes(); t.save(); return "OK";
        }
        if (cmd == "SELECT") {
            std::vector<std::pair<std::string,std::string>> out;
            bool aggregate = false;
            struct Agg { std::string fn, col, alias; }; std::vector<Agg> aggs;
            if (lx.eat("*")) {}
            else {
                lx.need("(");
                while (!lx.eat(")")) {
                    std::string first = lx.get();
                    if (lx.eat("(")) {
                        std::string col = lx.get(); lx.need(")");
                        std::string alias = upper(first) + "_" + col; if (lx.eat("AS")) alias = lx.get();
                        aggs.push_back({upper(first), col, alias}); aggregate = true;
                    } else {
                        std::string alias = first; if (lx.eat("AS")) alias = lx.get();
                        out.push_back({first, alias});
                    }
                    lx.eat(",");
                }
            }
            lx.need("FROM"); std::string name = lx.get(); Table t = openTable(name);
            if (out.empty() && !aggregate) for (auto& c : t.cols) out.push_back({c.name, c.name});
            std::string where; if (lx.eat("WHERE")) while (lx.has()) where += (where.empty() ? "" : " ") + lx.get();
            auto ids = filter(t, where);
            if (!aggregate) return jsonRows(t, ids, out);
            std::ostringstream o; o << "[{";
            for (size_t a = 0; a < aggs.size(); ++a) {
                if (a) o << ",";
                int c = t.colId(aggs[a].col); long long sum = 0, count = 0;
                for (size_t id : ids) if (t.rows[id][c].type == Value::Int) { sum += t.rows[id][c].i; ++count; }
                o << "\"" << aggs[a].alias << "\":";
                if (aggs[a].fn == "COUNT") o << ids.size();
                else if (aggs[a].fn == "SUM") o << sum;
                else if (aggs[a].fn == "AVG") o << (count ? (double)sum / count : 0);
                else throw Error("unknown aggregate: " + aggs[a].fn);
            }
            return o.str() + "}]";
        }
        if (cmd == "REVERT") {
            std::string name = lx.get(), ts = lx.get(); Table t = openTable(name); t.revert(ts); return "OK";
        }
        if (cmd == "TELEMETRY") return "[{\"mode\":\"local\",\"status\":\"available\",\"metric\":\"access log is stored in data/access.log\"}]";
        throw Error("unsupported command");
    } // Выполнение команд.
}; // Класс для выполнения команд.

static std::vector<std::string> readCommands(std::istream& in) {
    std::vector<std::string> cmds; std::string line, cur;
    while (std::getline(in, line)) {
        cur += line + "\n";
        size_t p;
        while ((p = cur.find(';')) != std::string::npos) {
            cmds.push_back(cur.substr(0, p + 1));
            cur.erase(0, p + 1);
        }
    }
    if (!trim(cur).empty()) throw Error("last command is not terminated by ';'");
    return cmds;
} // Для чтения команд.

int main(int argc, char** argv) {
    Engine e;
    try {
        if (argc > 2) { std::cerr << "Usage: cwdb [script.sql]\n"; return 2; }
        if (argc == 2) {
            std::ifstream f(argv[1]);
            if (!f) { std::cerr << "cannot open script: " << argv[1] << "\n"; return 2; }
            for (const auto& c : readCommands(f)) std::cout << e.exec(c) << "\n";
            return 0;
        }
        std::cout << "cwdb variant 2 (B+-tree). End commands with ;\n";
        std::string cur, line;
        while (std::cout << "> " && std::getline(std::cin, line)) {
            cur += line + "\n";
            size_t p;
            while ((p = cur.find(';')) != std::string::npos) {
                std::cout << e.exec(cur.substr(0, p + 1)) << "\n";
                cur.erase(0, p + 1);
            }
        }
    } catch (const std::exception& ex) {
        std::cerr << "Fatal error: " << ex.what() << "\n";
        return 1;
    }
    return 0;
} // Главная функция, которая заправляет всем.
