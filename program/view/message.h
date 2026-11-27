
#pragma once

#include "../program.h"

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

    auto warning(const std::string& text, const std::string& title = "") -> void {
        if (captured("warning", text))
            return;
        GUIKIT::MessageWindow()
            .setWindow(*window)
            .setTitle(title.empty() ? APP_NAME " " + trans->get("Warning") : title)
            .setText( text )
            .warning();
    }

    auto error(const std::string& text, const std::string& title = "") -> void {
        if (captured("error", text))
            return;
        GUIKIT::MessageWindow()
            .setWindow(*window)
            .setTitle(title.empty() ? APP_NAME " " + trans->get("Error") : title)
            .setText( text )
            .error();
    }

    auto information(const std::string& text, const std::string& title = "") -> void {
        if (captured("information", text))
            return;
        GUIKIT::MessageWindow()
            .setWindow(*window)
            .setTitle(title.empty() ? APP_NAME " " + trans->get("Information") : title)
            .setText( text )
            .information();
    }

    auto question(const std::string& text, const std::string& title = "") -> bool {
        if (captured("question (answered no)", text))
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
