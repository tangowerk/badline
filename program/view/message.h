#pragma once

#include "../program.h"
#include <cstdio>

struct Message;
extern Message* appMessage; // messages: stderr + on-screen text (no dialogs)

struct Message {
    // while set (UI thread, e.g. during an MCP call), messages are collected
    // here instead of being printed; questions are answered with 'no'
    static inline std::vector<std::string>* capture = nullptr;

    static auto captured(const std::string& kind, const std::string& text) -> bool {
        if (!capture)
            return false;
        capture->push_back(kind + ": " + text);
        return true;
    }

    // stderr + on-screen text, questions take the safe 'no'
    static auto print(const std::string& kind, const std::string& text, bool warn) -> void {
        if (captured(kind, text))
            return;
        std::fprintf(stderr, "%s: %s\n", kind.c_str(), text.c_str());
        showOnScreen(text, warn);
    }

    static auto showOnScreen(const std::string& text, bool warn) -> void;

    auto warning(const std::string& text, const std::string& title = "") -> void { print("warning", text, true); }
    auto error(const std::string& text, const std::string& title = "") -> void { print("error", text, true); }
    auto information(const std::string& text, const std::string& title = "") -> void { print("information", text, false); }
    auto question(const std::string& text, const std::string& title = "") -> bool {
        print("question (answered no)", text, true);
        return false;
    }
};
