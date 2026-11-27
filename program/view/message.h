
#pragma once

#include "../program.h"
#include <cstdio>

struct Message;
extern Message* appMessage; // window-independent messages (stderr + screen text without GTK)

struct Message {
    GUIKIT::Window* window;

    // while set (UI thread, e.g. during an MCP call), messages are collected
    // here instead of opening modal dialogs; questions are answered with 'no'
    static inline std::vector<std::string>* capture = nullptr;

    static auto captured(const std::string& kind, const std::string& text) -> bool {
        if (!capture)
            return false;
        capture->push_back(kind + ": " + text);
        return true;
    }

    // no GTK window (SDL window / headless): stderr + on-screen text, questions take the safe 'no'
    auto windowless(const std::string& kind, const std::string& text, bool warn) -> bool {
        if (window)
            return false;
        std::fprintf(stderr, "%s: %s\n", kind.c_str(), text.c_str());
        showOnScreen(text, warn);
        return true;
    }

    static auto showOnScreen(const std::string& text, bool warn) -> void;

    auto warning(const std::string& text, const std::string& title = "") -> void {
        if (captured("warning", text) || windowless("warning", text, true))
            return;
        GUIKIT::MessageWindow()
            .setWindow(*window)
            .setTitle(title.empty() ? APP_NAME " " + trans->get("Warning") : title)
            .setText( text )
            .warning();
    }

    auto error(const std::string& text, const std::string& title = "") -> void {
        if (captured("error", text) || windowless("error", text, true))
            return;
        GUIKIT::MessageWindow()
            .setWindow(*window)
            .setTitle(title.empty() ? APP_NAME " " + trans->get("Error") : title)
            .setText( text )
            .error();
    }

    auto information(const std::string& text, const std::string& title = "") -> void {
        if (captured("information", text) || windowless("information", text, false))
            return;
        GUIKIT::MessageWindow()
            .setWindow(*window)
            .setTitle(title.empty() ? APP_NAME " " + trans->get("Information") : title)
            .setText( text )
            .information();
    }

    auto question(const std::string& text, const std::string& title = "") -> bool {
        if (captured("question (answered no)", text) || windowless("question (answered no)", text, true))
            return false;
        return GUIKIT::MessageWindow()
            .setWindow(*window)
            .setTitle(title.empty() ? APP_NAME " " + trans->get("Question") : title )
            .setText( text )
            .question(GUIKIT::MessageWindow::Buttons::YesNo)
            == GUIKIT::MessageWindow::Response::Yes;
    }

    Message(GUIKIT::Window* window) : window(window) {}
};
