#pragma once
// A full-screen terminal menu without libraries (any distro's terminal): raw mode through
// termios, the alternate screen and ANSI codes. Arrow keys move, Space ticks a checkbox, ←/→
// change a value or choice, Enter acts, Esc or q goes back. The screen is redrawn whole on each
// change; menus are rebuilt from their state every time, so they always show what is true now.
#include <poll.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#include <algorithm>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace x4vr::tui {
enum Key { None = -1, Up = 1000, Down, Left, Right, Enter, Escape, Backspace, Home, End, PageUp, PageDown };

class Terminal {
public:
    Terminal() {
        ok_ = isatty(STDIN_FILENO) && isatty(STDOUT_FILENO) && tcgetattr(STDIN_FILENO, &saved_) == 0;
        if (!ok_) return;
        termios raw = saved_;
        raw.c_lflag &= ~tcflag_t(ICANON | ECHO | ISIG | IEXTEN);
        raw.c_iflag &= ~tcflag_t(IXON | ICRNL);
        raw.c_cc[VMIN] = 0;
        raw.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
        put("\x1b[?1049h\x1b[?25l"); // alternate screen, cursor hidden
    }
    ~Terminal() {
        if (!ok_) return;
        put("\x1b[0m\x1b[?25h\x1b[?1049l");
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved_);
    }
    Terminal(const Terminal&) = delete;
    Terminal& operator=(const Terminal&) = delete;
    bool ok() const { return ok_; }
    void put(const std::string& text) { ::write(STDOUT_FILENO, text.data(), text.size()); }
    int columns() const { winsize w{}; return ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0 && w.ws_col ? w.ws_col : 80; }
    int rows() const { winsize w{}; return ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0 && w.ws_row ? w.ws_row : 24; }
    // The next key, or None after `timeout_ms` (-1: wait).
    int key(int timeout_ms) {
        pollfd p{STDIN_FILENO, POLLIN, 0};
        if (poll(&p, 1, timeout_ms) <= 0) return None;
        unsigned char b[8]{};
        const auto n = ::read(STDIN_FILENO, b, sizeof b);
        if (n <= 0) return None;
        if (b[0] == 0x1b) {
            if (n == 1) return Escape;
            if (b[1] == '[' || b[1] == 'O') {
                switch (b[2]) {
                case 'A': return Up;   case 'B': return Down; case 'C': return Right; case 'D': return Left;
                case 'H': return Home; case 'F': return End;
                case '1': case '7': return Home; case '4': case '8': return End;
                case '5': return PageUp; case '6': return PageDown;
                }
            }
            return None;
        }
        if (b[0] == '\r' || b[0] == '\n') return Enter;
        if (b[0] == 127 || b[0] == 8) return Backspace;
        if (b[0] == 3) return 'q'; // Ctrl+C
        return b[0];
    }
private:
    bool ok_ = false;
    termios saved_{};
};

// One line of a menu.
struct Item {
    enum Kind { Heading, Text, Status, Toggle, Choice, Number, Action } kind = Text;
    std::string id, label, value, help;
    int status = 0;                      // Status: 0 ok, 1 warning, 2 problem, 3 unknown
    bool on = false;                     // Toggle
    std::vector<std::string> choices;    // Choice
    int choice = 0;
    double number = 0, step = 0.1, low = 0, high = 100; // Number
    int decimals = 2;
    bool selectable() const { return kind == Toggle || kind == Choice || kind == Number || kind == Action; }
};
inline Item heading(std::string label) { Item i; i.kind = Item::Heading; i.label = std::move(label); return i; }
inline Item text(std::string label) { Item i; i.kind = Item::Text; i.label = std::move(label); return i; }
inline Item status(std::string label, int state, std::string value, std::string help = {}) {
    Item i; i.kind = Item::Status; i.label = std::move(label); i.status = state; i.value = std::move(value); i.help = std::move(help); return i;
}
inline Item action(std::string id, std::string label, std::string help = {}) {
    Item i; i.kind = Item::Action; i.id = std::move(id); i.label = std::move(label); i.help = std::move(help); return i;
}
inline Item toggle(std::string id, std::string label, bool on, std::string help = {}) {
    Item i; i.kind = Item::Toggle; i.id = std::move(id); i.label = std::move(label); i.on = on; i.help = std::move(help); return i;
}
inline Item choice(std::string id, std::string label, std::vector<std::string> choices, int current, std::string help = {}) {
    Item i; i.kind = Item::Choice; i.id = std::move(id); i.label = std::move(label); i.choices = std::move(choices);
    i.choice = current; i.help = std::move(help); return i;
}
inline Item number(std::string id, std::string label, double value, double step, double low, double high, int decimals, std::string help = {}) {
    Item i; i.kind = Item::Number; i.id = std::move(id); i.label = std::move(label); i.number = value; i.step = step;
    i.low = low; i.high = high; i.decimals = decimals; i.help = std::move(help); return i;
}

inline std::string format(double value, int decimals) { char t[32]; std::snprintf(t, sizeof t, "%.*f", decimals, value); return t; }
// Display width of UTF-8 text (one column per code point; the menu only uses narrow symbols).
inline size_t width(const std::string& s) { return size_t(std::count_if(s.begin(), s.end(), [](char c) { return (c & 0xc0) != 0x80; })); }
inline std::string fit(const std::string& s, size_t columns) {
    if (width(s) <= columns) return s+std::string(columns-width(s), ' ');
    std::string out; size_t w = 0;
    for (size_t i = 0; i < s.size() && w+1 < columns; ++i) { out += s[i]; if ((s[i] & 0xc0) != 0x80) ++w; while (i+1 < s.size() && (s[i+1] & 0xc0) == 0x80) out += s[++i]; }
    return out+"…";
}

// What happened on a menu: the item acted on (id) and how.
struct Event { enum Kind { Back, Activate, Changed } kind; std::string id; Item item; };

// Draws `items` with the cursor on `cursor` (an index into items), the title, a message line
// and the selected item's help, and handles one key. Returns an event, or nothing for navigation.
class Menu {
public:
    explicit Menu(Terminal& t) : t_(t) {}
    std::string title, footer = "↑↓ move   Enter select   Space tick   ←→ change   Esc back";
    std::vector<std::string> messages; // shown under the items until the next key
    int timeout_ms = -1;               // redraw without a key after this (-1: wait), for live status

    bool step(std::vector<Item>& items, Event& event) {
        clamp(items);
        draw(items);
        const int k = t_.key(timeout_ms);
        if (k == None) return false;
        messages.clear();
        if (k == 'q' || k == Escape) { event = {Event::Back, {}, {}}; return true; }
        if (k == Up || k == 'k') move(items, -1);
        else if (k == Down || k == 'j' || k == '\t') move(items, +1);
        else if (k == Home || k == PageUp) { cursor_ = 0; clamp(items); }
        else if (k == End || k == PageDown) { cursor_ = int(items.size())-1; move(items, -1); move(items, +1); }
        else if (cursor_ >= 0 && cursor_ < int(items.size())) {
            auto& item = items[size_t(cursor_)];
            if (item.kind == Item::Action && (k == Enter || k == ' ')) { event = {Event::Activate, item.id, item}; return true; }
            if (item.kind == Item::Toggle && (k == Enter || k == ' ')) { item.on = !item.on; event = {Event::Changed, item.id, item}; return true; }
            if (item.kind == Item::Choice && (k == Left || k == Right || k == Enter || k == ' ')) {
                const int n = int(item.choices.size());
                item.choice = (item.choice+(k == Left ? n-1 : 1))%n;
                event = {Event::Changed, item.id, item};
                return true;
            }
            if (item.kind == Item::Number && (k == Left || k == Right)) {
                item.number = std::clamp(item.number+(k == Left ? -item.step : item.step), item.low, item.high);
                event = {Event::Changed, item.id, item};
                return true;
            }
            if (item.kind == Item::Number && k == Enter) {
                std::string typed;
                if (edit(items, item, typed)) {
                    try {
                        item.number = std::clamp(std::stod(typed), item.low, item.high);
                        event = {Event::Changed, item.id, item};
                        return true;
                    } catch (...) { messages.push_back("Not a number: "+typed); }
                }
            }
        }
        return false;
    }
    int cursor() const { return cursor_; }
    void select(const std::vector<Item>& items, const std::string& id) {
        for (size_t i = 0; i < items.size(); ++i) if (items[i].id == id) cursor_ = int(i);
    }

private:
    Terminal& t_;
    int cursor_ = -1;
    void clamp(const std::vector<Item>& items) {
        if (items.empty()) { cursor_ = -1; return; }
        cursor_ = std::clamp(cursor_, 0, int(items.size())-1);
        if (!items[size_t(cursor_)].selectable()) { move(items, +1); if (!items[size_t(cursor_)].selectable()) move(items, -1); }
    }
    void move(const std::vector<Item>& items, int direction) {
        for (int i = cursor_+direction; i >= 0 && i < int(items.size()); i += direction)
            if (items[size_t(i)].selectable()) { cursor_ = i; return; }
    }
    std::string line(const Item& item, bool selected, size_t columns) const {
        std::string left, right;
        switch (item.kind) {
        case Item::Heading: return "\x1b[1m"+fit(item.label, columns)+"\x1b[0m";
        case Item::Text: return fit("  "+item.label, columns);
        case Item::Status: {
            static const char* marks[] = {"\x1b[32m✓\x1b[0m", "\x1b[33m!\x1b[0m", "\x1b[31m✗\x1b[0m", "\x1b[90m?\x1b[0m"};
            const auto label = fit(item.label, 34);
            return "  "+std::string(marks[std::clamp(item.status, 0, 3)])+" "+label+" "+fit(item.value, columns > 40 ? columns-40 : 0);
        }
        case Item::Toggle: left = std::string(item.on ? "[x] " : "[ ] ")+item.label; break;
        case Item::Choice: left = item.label; right = "‹ "+item.choices[size_t(item.choice)]+" ›"; break;
        case Item::Number: left = item.label; right = "‹ "+format(item.number, item.decimals)+" ›"; break;
        case Item::Action: left = "▸ "+item.label; break;
        }
        std::string text = "  "+left;
        if (!right.empty()) text = fit(text, 40)+right;
        text = fit(text, columns);
        return selected ? "\x1b[7m"+text+"\x1b[0m" : text;
    }
    void draw(const std::vector<Item>& items) {
        const size_t columns = size_t(std::max(t_.columns(), 20));
        const int rows = std::max(t_.rows(), 10);
        std::string out = "\x1b[H\x1b[2J\x1b[1;7m"+fit(" "+title, columns)+"\x1b[0m\r\n\r\n";
        // Scroll so the cursor stays visible: items, then messages and help, then the footer.
        const int reserved = 4+int(messages.size())+3;
        const int visible = std::max(rows-reserved, 3);
        int first = 0;
        if (cursor_ >= visible) first = cursor_-visible+1;
        for (int i = first; i < int(items.size()) && i < first+visible; ++i)
            out += line(items[size_t(i)], i == cursor_, columns)+"\r\n";
        out += "\r\n";
        for (const auto& m : messages) out += "\x1b[1m"+fit(" "+m, columns)+"\x1b[0m\r\n";
        if (cursor_ >= 0 && cursor_ < int(items.size()) && !items[size_t(cursor_)].help.empty())
            out += "\x1b[90m"+fit(" "+items[size_t(cursor_)].help, columns)+"\x1b[0m\r\n";
        out += "\x1b["+std::to_string(rows)+";1H\x1b[90m"+fit(" "+footer, columns)+"\x1b[0m";
        t_.put(out);
    }
    // Typed value for a number; false if cancelled.
    bool edit(std::vector<Item>& items, const Item& item, std::string& typed) {
        for (;;) {
            messages = {item.label+": "+typed+"_   (Enter to set, Esc to cancel)"};
            draw(items);
            const int k = t_.key(-1);
            if (k == Escape) { messages.clear(); return false; }
            if (k == Enter) { messages.clear(); return !typed.empty(); }
            if (k == Backspace) { if (!typed.empty()) typed.pop_back(); }
            else if ((k >= '0' && k <= '9') || k == '.' || k == ',' || k == '-') typed += char(k == ',' ? '.' : k);
        }
    }
};
}
