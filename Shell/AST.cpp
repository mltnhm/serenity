/*
 * Copyright (c) 2020, the SerenityOS developers.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 *    list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "AST.h"
#include "Shell.h"
#include <AK/String.h>
#include <AK/StringBuilder.h>
#include <AK/URL.h>
#include <LibCore/File.h>

//#define EXECUTE_DEBUG

namespace AST {

template<typename T, typename... Args>
static inline RefPtr<T> create(Args... args)
{
    return adopt(*new T(args...));
}

template<typename T>
static inline RefPtr<T> create(std::initializer_list<RefPtr<Value>> arg)
{
    return adopt(*new T(arg));
}

static inline void print_indented(const String& str, int indent)
{
    for (auto i = 0; i < indent; ++i)
        dbgprintf("  ");
    dbgprintf("%s\n", str.characters());
}

static inline Vector<Command> join_commands(Vector<Command> left, Vector<Command> right)
{
    Command command;

    auto last_in_left = left.take_last();
    auto first_in_right = right.take_first();

    command.argv.append(last_in_left.argv);
    command.argv.append(first_in_right.argv);

    command.redirections.append(last_in_left.redirections);
    command.redirections.append(first_in_right.redirections);

    command.should_wait = first_in_right.should_wait && last_in_left.should_wait;
    command.is_pipe_source = first_in_right.is_pipe_source;
    command.should_notify_if_in_background = first_in_right.should_wait && last_in_left.should_notify_if_in_background;

    Vector<Command> commands;
    commands.append(left);
    commands.append(command);
    commands.append(right);

    return commands;
}

void Node::dump(int level) const
{
    print_indented(String::format("%s at %d:%d", class_name().characters(), m_position.start_offset, m_position.end_offset), level);
}

Node::Node(Position position)
    : m_position(position)
{
}

Vector<Line::CompletionSuggestion> Node::complete_for_editor(Shell& shell, size_t offset, const HitTestResult& hit_test_result)
{
    auto matching_node = hit_test_result.matching_node;
    if (matching_node) {
        if (matching_node->is_bareword()) {
            auto corrected_offset = offset - matching_node->position().start_offset;
            auto* node = static_cast<BarewordLiteral*>(matching_node.ptr());

            if (corrected_offset > node->text().length())
                return {};
            auto& text = node->text();

            // If the literal isn't an option, treat it as a path.
            if (!(text.starts_with("-") || text == "--" || text == "-"))
                return shell.complete_path("", text, corrected_offset);

            // If the literal is an option, we have to know the program name
            // should we have no way to get that, bail early.

            if (!hit_test_result.closest_command_node)
                return {};

            auto program_name_node = hit_test_result.closest_command_node->leftmost_trivial_literal();
            if (!program_name_node)
                return {};

            String program_name;
            if (program_name_node->is_bareword())
                program_name = static_cast<BarewordLiteral*>(program_name_node.ptr())->text();
            else
                program_name = static_cast<StringLiteral*>(program_name_node.ptr())->text();

            return shell.complete_option(program_name, text, corrected_offset);
        }
        return {};
    }
    auto result = hit_test_position(offset);
    if (!result.matching_node)
        return {};
    auto node = result.matching_node;
    if (node->is_bareword() || node != result.closest_node_with_semantic_meaning)
        node = result.closest_node_with_semantic_meaning;

    if (!node)
        return {};

    return node->complete_for_editor(shell, offset, result);
}

Vector<Line::CompletionSuggestion> Node::complete_for_editor(Shell& shell, size_t offset)
{
    return Node::complete_for_editor(shell, offset, { nullptr, nullptr, nullptr });
}

Node::~Node()
{
}

void And::dump(int level) const
{
    Node::dump(level);
    m_left->dump(level + 1);
    m_right->dump(level + 1);
}

RefPtr<Value> And::run(RefPtr<Shell> shell)
{
    auto left = m_left->run(shell);
    ASSERT(left->is_job());

    auto* job_value = static_cast<JobValue*>(left.ptr());
    const auto job = job_value->job();
    if (!job) {
        // Something has gone wrong, let's just pretend that the job failed.
        return job_value;
    }

    shell->block_on_job(job);

    if (job->exit_code() == 0)
        return m_right->run(shell);

    return job_value;
}

void And::highlight_in_editor(Line::Editor& editor, Shell& shell, HighlightMetadata metadata)
{
    metadata.is_first_in_list = true;
    m_left->highlight_in_editor(editor, shell, metadata);
    m_right->highlight_in_editor(editor, shell, metadata);
}

HitTestResult And::hit_test_position(size_t offset)
{
    if (!position().contains(offset))
        return {};

    auto result = m_left->hit_test_position(offset);
    if (result.matching_node) {
        if (!result.closest_command_node)
            result.closest_command_node = m_right;
        return result;
    }
    result = m_right->hit_test_position(offset);
    if (!result.closest_command_node)
        result.closest_command_node = m_right;
    return result;
}

And::And(Position position, RefPtr<Node> left, RefPtr<Node> right)
    : Node(move(position))
    , m_left(move(left))
    , m_right(move(right))
{
    if (m_left->is_syntax_error())
        set_is_syntax_error(m_left->syntax_error_node());
    else if (m_right->is_syntax_error())
        set_is_syntax_error(m_right->syntax_error_node());
}

And::~And()
{
}

void ListConcatenate::dump(int level) const
{
    Node::dump(level);
    m_element->dump(level + 1);
    m_list->dump(level + 1);
}

RefPtr<Value> ListConcatenate::run(RefPtr<Shell> shell)
{
    auto list = m_list->run(shell)->resolve_without_cast(shell);
    auto element = m_element->run(shell)->resolve_without_cast(shell);

    if (list->is_command() || element->is_command()) {
        auto joined_commands = join_commands(element->resolve_as_commands(shell), list->resolve_as_commands(shell));

        if (joined_commands.size() == 1)
            return create<CommandValue>(joined_commands[0]);
        return create<CommandSequenceValue>(move(joined_commands));
    }

    return create<ListValue>({ move(element), move(list) });
}

void ListConcatenate::highlight_in_editor(Line::Editor& editor, Shell& shell, HighlightMetadata metadata)
{
    auto first = metadata.is_first_in_list;
    metadata.is_first_in_list = false;
    m_list->highlight_in_editor(editor, shell, metadata);
    metadata.is_first_in_list = first;
    m_element->highlight_in_editor(editor, shell, metadata);
}

HitTestResult ListConcatenate::hit_test_position(size_t offset)
{
    if (!position().contains(offset))
        return {};

    auto result = m_element->hit_test_position(offset);
    if (result.matching_node)
        return result;
    result = m_list->hit_test_position(offset);
    if (!result.closest_node_with_semantic_meaning)
        result.closest_node_with_semantic_meaning = this;
    return result;
}

RefPtr<Node> ListConcatenate::leftmost_trivial_literal() const
{
    return m_element->leftmost_trivial_literal();
}

ListConcatenate::ListConcatenate(Position position, RefPtr<Node> element, RefPtr<Node> list)
    : Node(move(position))
    , m_element(move(element))
    , m_list(move(list))
{
    if (m_element->is_syntax_error())
        set_is_syntax_error(m_element->syntax_error_node());
    else if (m_list->is_syntax_error())
        set_is_syntax_error(m_list->syntax_error_node());
}

ListConcatenate::~ListConcatenate()
{
}

void Background::dump(int level) const
{
    Node::dump(level);
    m_command->dump(level + 1);
}

RefPtr<Value> Background::run(RefPtr<Shell> shell)
{
    auto commands = m_command->run(shell)->resolve_as_commands(shell);
    auto& last = commands.last();
    last.should_wait = false;

    return create<CommandSequenceValue>(move(commands));
}

void Background::highlight_in_editor(Line::Editor& editor, Shell& shell, HighlightMetadata metadata)
{
    m_command->highlight_in_editor(editor, shell, metadata);
}

HitTestResult Background::hit_test_position(size_t offset)
{
    if (!position().contains(offset))
        return {};

    return m_command->hit_test_position(offset);
}

Background::Background(Position position, RefPtr<Node> command)
    : Node(move(position))
    , m_command(move(command))
{
    if (m_command->is_syntax_error())
        set_is_syntax_error(m_command->syntax_error_node());
}

Background::~Background()
{
}

void BarewordLiteral::dump(int level) const
{
    Node::dump(level);
    print_indented(m_text, level + 1);
}

RefPtr<Value> BarewordLiteral::run(RefPtr<Shell>)
{
    return create<StringValue>(m_text);
}

void BarewordLiteral::highlight_in_editor(Line::Editor& editor, Shell& shell, HighlightMetadata metadata)
{
    if (metadata.is_first_in_list) {
        editor.stylize({ m_position.start_offset, m_position.end_offset }, { Line::Style::Bold });
        return;
    }
    if (m_text.starts_with('-')) {
        if (m_text == "--") {
            editor.stylize({ m_position.start_offset, m_position.end_offset }, { Line::Style::Foreground(Line::Style::XtermColor::Green) });
            return;
        }
        if (m_text == "-")
            return;

        if (m_text.starts_with("--")) {
            auto index = m_text.index_of("=").value_or(m_text.length() - 1) + 1;
            editor.stylize({ m_position.start_offset, m_position.start_offset + index }, { Line::Style::Foreground(Line::Style::XtermColor::Cyan) });
        } else {
            editor.stylize({ m_position.start_offset, m_position.end_offset }, { Line::Style::Foreground(Line::Style::XtermColor::Cyan) });
        }
    }
    if (Core::File::exists(m_text)) {
        auto realpath = shell.resolve_path(m_text);
        auto url = URL::create_with_file_protocol(realpath);
        url.set_host(shell.hostname);
        editor.stylize({ m_position.start_offset, m_position.end_offset }, { Line::Style::Hyperlink(url.to_string()) });
    }
}

BarewordLiteral::BarewordLiteral(Position position, String text)
    : Node(move(position))
    , m_text(move(text))
{
}

BarewordLiteral::~BarewordLiteral()
{
}

void CastToCommand::dump(int level) const
{
    Node::dump(level);
    m_inner->dump(level + 1);
}

RefPtr<Value> CastToCommand::run(RefPtr<Shell> shell)
{
    if (m_inner->is_command())
        return m_inner->run(shell);

    auto value = m_inner->run(shell)->resolve_without_cast(shell);
    if (value->is_command())
        return value;

    auto argv = value->resolve_as_list(shell);
    return create<CommandValue>(move(argv));
}

void CastToCommand::highlight_in_editor(Line::Editor& editor, Shell& shell, HighlightMetadata metadata)
{
    m_inner->highlight_in_editor(editor, shell, metadata);
}

HitTestResult CastToCommand::hit_test_position(size_t offset)
{
    if (!position().contains(offset))
        return {};

    auto result = m_inner->hit_test_position(offset);
    if (!result.closest_node_with_semantic_meaning)
        result.closest_node_with_semantic_meaning = this;
    return result;
}

Vector<Line::CompletionSuggestion> CastToCommand::complete_for_editor(Shell& shell, size_t offset, const HitTestResult& hit_test_result)
{
    auto matching_node = hit_test_result.matching_node;
    if (!matching_node)
        return {};

    ASSERT(matching_node->is_bareword());
    auto corrected_offset = offset - matching_node->position().start_offset;
    auto* node = static_cast<BarewordLiteral*>(matching_node.ptr());

    if (corrected_offset > node->text().length())
        return {};

    return shell.complete_program_name(node->text(), corrected_offset);
}

RefPtr<Node> CastToCommand::leftmost_trivial_literal() const
{
    return m_inner->leftmost_trivial_literal();
}

CastToCommand::CastToCommand(Position position, RefPtr<Node> inner)
    : Node(move(position))
    , m_inner(move(inner))
{
    if (m_inner->is_syntax_error())
        set_is_syntax_error(m_inner->syntax_error_node());
}

CastToCommand::~CastToCommand()
{
}

void CastToList::dump(int level) const
{
    Node::dump(level);
    if (m_inner)
        m_inner->dump(level + 1);
    else
        print_indented("(empty)", level + 1);
}

RefPtr<Value> CastToList::run(RefPtr<Shell> shell)
{
    if (!m_inner)
        return create<ListValue>({});

    auto inner_value = m_inner->run(shell);

    if (inner_value->is_command())
        return inner_value;

    auto values = inner_value->resolve_as_list(shell);
    Vector<RefPtr<Value>> cast_values;
    for (auto& value : values)
        cast_values.append(create<StringValue>(value));

    return create<ListValue>(cast_values);
}

void CastToList::highlight_in_editor(Line::Editor& editor, Shell& shell, HighlightMetadata metadata)
{
    if (m_inner)
        m_inner->highlight_in_editor(editor, shell, metadata);
}

HitTestResult CastToList::hit_test_position(size_t offset)
{
    if (!position().contains(offset))
        return {};

    if (!m_inner)
        return {};

    return m_inner->hit_test_position(offset);
}

RefPtr<Node> CastToList::leftmost_trivial_literal() const
{
    return m_inner->leftmost_trivial_literal();
}

CastToList::CastToList(Position position, RefPtr<Node> inner)
    : Node(move(position))
    , m_inner(move(inner))
{
    if (m_inner && m_inner->is_syntax_error())
        set_is_syntax_error(m_inner->syntax_error_node());
}

CastToList::~CastToList()
{
}

void CloseFdRedirection::dump(int level) const
{
    Node::dump(level);
    print_indented(String::format("%d -> Close", m_fd), level);
}

RefPtr<Value> CloseFdRedirection::run(RefPtr<Shell>)
{
    Command command;
    command.redirections.append(*new CloseRedirection(m_fd));
    return create<CommandValue>(move(command));
}

void CloseFdRedirection::highlight_in_editor(Line::Editor& editor, Shell&, HighlightMetadata)
{
    editor.stylize({ m_position.start_offset, m_position.end_offset - 1 }, { Line::Style::Foreground(0x87, 0x9b, 0xcd) }); // 25% Darkened Periwinkle
    editor.stylize({ m_position.end_offset - 1, m_position.end_offset }, { Line::Style::Foreground(0xff, 0x7e, 0x00) });   // Amber
}

CloseFdRedirection::CloseFdRedirection(Position position, int fd)
    : Node(move(position))
    , m_fd(fd)
{
}

CloseFdRedirection::~CloseFdRedirection()
{
}

void CommandLiteral::dump(int level) const
{
    Node::dump(level);
    print_indented("(Generated command literal)", level + 1);
}

RefPtr<Value> CommandLiteral::run(RefPtr<Shell>)
{
    return create<CommandValue>(m_command);
}

CommandLiteral::CommandLiteral(Position position, Command command)
    : Node(move(position))
    , m_command(move(command))
{
}

CommandLiteral::~CommandLiteral()
{
}

void Comment::dump(int level) const
{
    Node::dump(level);
    print_indented(m_text, level + 1);
}

RefPtr<Value> Comment::run(RefPtr<Shell>)
{
    return create<ListValue>({});
}

void Comment::highlight_in_editor(Line::Editor& editor, Shell&, HighlightMetadata)
{
    editor.stylize({ m_position.start_offset, m_position.end_offset }, { Line::Style::Foreground(150, 150, 150) }); // Light gray
}

Comment::Comment(Position position, String text)
    : Node(move(position))
    , m_text(move(text))
{
}

Comment::~Comment()
{
}

void DoubleQuotedString::dump(int level) const
{
    Node::dump(level);
    m_inner->dump(level + 1);
}

RefPtr<Value> DoubleQuotedString::run(RefPtr<Shell> shell)
{
    StringBuilder builder;
    auto values = m_inner->run(shell)->resolve_as_list(shell);

    builder.join("", values);

    return create<StringValue>(builder.to_string());
}

void DoubleQuotedString::highlight_in_editor(Line::Editor& editor, Shell& shell, HighlightMetadata metadata)
{
    Line::Style style { Line::Style::Foreground(Line::Style::XtermColor::Yellow) };
    if (metadata.is_first_in_list)
        style.unify_with({ Line::Style::Bold });

    editor.stylize({ m_position.start_offset, m_position.end_offset }, style);
    metadata.is_first_in_list = false;
    m_inner->highlight_in_editor(editor, shell, metadata);
}

HitTestResult DoubleQuotedString::hit_test_position(size_t offset)
{
    if (!position().contains(offset))
        return {};

    return m_inner->hit_test_position(offset);
}

DoubleQuotedString::DoubleQuotedString(Position position, RefPtr<Node> inner)
    : Node(move(position))
    , m_inner(move(inner))
{
    if (m_inner->is_syntax_error())
        set_is_syntax_error(m_inner->syntax_error_node());
}

DoubleQuotedString::~DoubleQuotedString()
{
}

void DynamicEvaluate::dump(int level) const
{
    Node::dump(level);
    m_inner->dump(level + 1);
}

RefPtr<Value> DynamicEvaluate::run(RefPtr<Shell> shell)
{
    auto result = m_inner->run(shell)->resolve_without_cast(shell);
    // Dynamic Evaluation behaves differently between strings and lists.
    // Strings are treated as variables, and Lists are treated as commands.
    if (result->is_string()) {
        auto name_part = result->resolve_as_list(shell);
        ASSERT(name_part.size() == 1);
        return create<SimpleVariableValue>(name_part[0]);
    }

    // If it's anything else, we're just gonna cast it to a list.
    auto list = result->resolve_as_list(shell);
    return create<CommandValue>(move(list));
}

void DynamicEvaluate::highlight_in_editor(Line::Editor& editor, Shell& shell, HighlightMetadata metadata)
{
    editor.stylize({ m_position.start_offset, m_position.end_offset }, { Line::Style::Foreground(Line::Style::XtermColor::Yellow) });
    m_inner->highlight_in_editor(editor, shell, metadata);
}

HitTestResult DynamicEvaluate::hit_test_position(size_t offset)
{
    if (!position().contains(offset))
        return {};

    return m_inner->hit_test_position(offset);
}

DynamicEvaluate::DynamicEvaluate(Position position, RefPtr<Node> inner)
    : Node(move(position))
    , m_inner(move(inner))
{
    if (m_inner->is_syntax_error())
        set_is_syntax_error(m_inner->syntax_error_node());
}

DynamicEvaluate::~DynamicEvaluate()
{
}

void Fd2FdRedirection::dump(int level) const
{
    Node::dump(level);
    print_indented(String::format("%d -> %d", source_fd, dest_fd), level);
}

RefPtr<Value> Fd2FdRedirection::run(RefPtr<Shell>)
{
    Command command;
    command.redirections.append(*new FdRedirection(source_fd, dest_fd, Rewiring::Close::None));
    return create<CommandValue>(move(command));
}

void Fd2FdRedirection::highlight_in_editor(Line::Editor& editor, Shell&, HighlightMetadata)
{
    editor.stylize({ m_position.start_offset, m_position.end_offset }, { Line::Style::Foreground(0x87, 0x9b, 0xcd) }); // 25% Darkened Periwinkle
}

Fd2FdRedirection::Fd2FdRedirection(Position position, int src, int dst)
    : Node(move(position))
    , source_fd(src)
    , dest_fd(dst)
{
}

Fd2FdRedirection::~Fd2FdRedirection()
{
}

void Glob::dump(int level) const
{
    Node::dump(level);
    print_indented(m_text, level + 1);
}

RefPtr<Value> Glob::run(RefPtr<Shell>)
{
    return create<GlobValue>(m_text);
}

void Glob::highlight_in_editor(Line::Editor& editor, Shell&, HighlightMetadata metadata)
{
    Line::Style style { Line::Style::Foreground(Line::Style::XtermColor::Cyan) };
    if (metadata.is_first_in_list)
        style.unify_with({ Line::Style::Bold });
    editor.stylize({ m_position.start_offset, m_position.end_offset }, move(style));
}

Glob::Glob(Position position, String text)
    : Node(move(position))
    , m_text(move(text))
{
}

Glob::~Glob()
{
}

void Execute::dump(int level) const
{
    Node::dump(level);
    if (m_capture_stdout)
        print_indented("(Capturing stdout)", level + 1);
    m_command->dump(level + 1);
}

RefPtr<Value> Execute::run(RefPtr<Shell> shell)
{
    RefPtr<Job> job;

    if (m_command->would_execute())
        return m_command->run(shell);

    auto commands = shell->expand_aliases(m_command->run(shell)->resolve_as_commands(shell));

    if (m_capture_stdout) {
        int pipefd[2];
        int rc = pipe(pipefd);
        if (rc < 0) {
            dbg() << "Error: cannot pipe(): " << strerror(errno);
            return create<StringValue>("");
        }
        auto& last_in_commands = commands.last();

        last_in_commands.redirections.prepend(*new FdRedirection(STDOUT_FILENO, pipefd[1], Rewiring::Close::Destination));
        last_in_commands.should_wait = true;
        last_in_commands.should_notify_if_in_background = false;
        last_in_commands.is_pipe_source = false;

        auto notifier = Core::Notifier::construct(pipefd[0], Core::Notifier::Read);
        StringBuilder builder;

        auto try_read = [&] {
            u8 buffer[4096];
            size_t remaining_size = 4096;
            for (;;) {
                if (remaining_size == 0)
                    break;
                auto read_size = read(pipefd[0], buffer, remaining_size);
                if (read_size < 0) {
                    if (errno == EINTR)
                        continue;
                    if (errno == 0)
                        break;
                    dbg() << "read() failed: " << strerror(errno);
                    break;
                }
                if (read_size == 0)
                    break;
                remaining_size -= read_size;
            }

            builder.append(StringView { buffer, 4096 - remaining_size });
        };

        notifier->on_ready_to_read = [&] {
            try_read();
        };

        for (auto job : shell->run_commands(commands)) {
            shell->block_on_job(job);
        }

        notifier->on_ready_to_read = nullptr;

        try_read();

        if (close(pipefd[0]) < 0) {
            dbg() << "close() failed: " << strerror(errno);
        }

        return create<StringValue>(builder.build(), shell->local_variable_or("IFS", "\n"), shell->options.inline_exec_keep_empty_segments);
    }

    RefPtr<Job> last_job;

    for (auto& job : shell->run_commands(commands)) {
        shell->block_on_job(job);
        last_job = move(job);
    }

    return create<JobValue>(move(last_job));
}

void Execute::highlight_in_editor(Line::Editor& editor, Shell& shell, HighlightMetadata metadata)
{
    if (m_capture_stdout)
        editor.stylize({ m_position.start_offset, m_position.end_offset }, { Line::Style::Foreground(Line::Style::XtermColor::Green) });
    metadata.is_first_in_list = true;
    m_command->highlight_in_editor(editor, shell, metadata);
}

HitTestResult Execute::hit_test_position(size_t offset)
{
    if (!position().contains(offset))
        return {};

    auto result = m_command->hit_test_position(offset);
    if (!result.closest_node_with_semantic_meaning)
        result.closest_node_with_semantic_meaning = this;
    if (!result.closest_command_node)
        result.closest_command_node = m_command;
    return result;
}

Vector<Line::CompletionSuggestion> Execute::complete_for_editor(Shell& shell, size_t offset, const HitTestResult& hit_test_result)
{
    auto matching_node = hit_test_result.matching_node;
    if (!matching_node)
        return {};

    ASSERT(matching_node->is_bareword());
    auto corrected_offset = offset - matching_node->position().start_offset;
    auto* node = static_cast<BarewordLiteral*>(matching_node.ptr());

    if (corrected_offset > node->text().length())
        return {};

    return shell.complete_program_name(node->text(), corrected_offset);
}

Execute::Execute(Position position, RefPtr<Node> command, bool capture_stdout)
    : Node(move(position))
    , m_command(move(command))
    , m_capture_stdout(capture_stdout)
{
    if (m_command->is_syntax_error())
        set_is_syntax_error(m_command->syntax_error_node());
}

Execute::~Execute()
{
}

void Join::dump(int level) const
{
    Node::dump(level);
    m_left->dump(level + 1);
    m_right->dump(level + 1);
}

RefPtr<Value> Join::run(RefPtr<Shell> shell)
{
    auto left = m_left->run(shell)->resolve_as_commands(shell);
    auto right = m_right->run(shell)->resolve_as_commands(shell);

    return create<CommandSequenceValue>(join_commands(move(left), move(right)));
}

void Join::highlight_in_editor(Line::Editor& editor, Shell& shell, HighlightMetadata metadata)
{
    m_left->highlight_in_editor(editor, shell, metadata);
    if (m_left->is_list() || m_left->is_command())
        metadata.is_first_in_list = false;
    m_right->highlight_in_editor(editor, shell, metadata);
}

HitTestResult Join::hit_test_position(size_t offset)
{
    if (!position().contains(offset))
        return {};

    auto result = m_left->hit_test_position(offset);
    if (result.matching_node)
        return result;
    return m_right->hit_test_position(offset);
}

RefPtr<Node> Join::leftmost_trivial_literal() const
{
    if (auto value = m_left->leftmost_trivial_literal())
        return value;
    return m_right->leftmost_trivial_literal();
}

Join::Join(Position position, RefPtr<Node> left, RefPtr<Node> right)
    : Node(move(position))
    , m_left(move(left))
    , m_right(move(right))
{
    if (m_left->is_syntax_error())
        set_is_syntax_error(m_left->syntax_error_node());
    else if (m_right->is_syntax_error())
        set_is_syntax_error(m_right->syntax_error_node());
}

Join::~Join()
{
}

void Or::dump(int level) const
{
    Node::dump(level);
    m_left->dump(level + 1);
    m_right->dump(level + 1);
}

RefPtr<Value> Or::run(RefPtr<Shell> shell)
{

    auto left = m_left->run(shell);
    ASSERT(left->is_job());

    auto* job_value = static_cast<JobValue*>(left.ptr());
    const auto job = job_value->job();
    if (!job) {
        // Something has gone wrong, let's just pretend that the job failed.
        return m_right->run(shell);
    }

    shell->block_on_job(job);

    if (job->exit_code() == 0)
        return job_value;

    return m_right->run(shell);
}

void Or::highlight_in_editor(Line::Editor& editor, Shell& shell, HighlightMetadata metadata)
{
    m_left->highlight_in_editor(editor, shell, metadata);
    m_right->highlight_in_editor(editor, shell, metadata);
}

HitTestResult Or::hit_test_position(size_t offset)
{
    if (!position().contains(offset))
        return {};

    auto result = m_left->hit_test_position(offset);
    if (result.matching_node) {
        if (!result.closest_command_node)
            result.closest_command_node = m_right;
        return result;
    }
    result = m_right->hit_test_position(offset);
    if (!result.closest_command_node)
        result.closest_command_node = m_right;
    return result;
}

Or::Or(Position position, RefPtr<Node> left, RefPtr<Node> right)
    : Node(move(position))
    , m_left(move(left))
    , m_right(move(right))
{
    if (m_left->is_syntax_error())
        set_is_syntax_error(m_left->syntax_error_node());
    else if (m_right->is_syntax_error())
        set_is_syntax_error(m_right->syntax_error_node());
}

Or::~Or()
{
}

void Pipe::dump(int level) const
{
    Node::dump(level);
    m_left->dump(level + 1);
    m_right->dump(level + 1);
}

RefPtr<Value> Pipe::run(RefPtr<Shell> shell)
{
    auto left = m_left->run(shell)->resolve_as_commands(shell);
    auto right = m_right->run(shell)->resolve_as_commands(shell);

    auto last_in_left = left.take_last();
    auto first_in_right = right.take_first();

    auto pipe_write_end = new FdRedirection(STDIN_FILENO, -1, Rewiring::Close::Destination);
    auto pipe_read_end = new FdRedirection(STDOUT_FILENO, -1, pipe_write_end, Rewiring::Close::RefreshDestination);
    first_in_right.redirections.append(*pipe_write_end);
    last_in_left.redirections.append(*pipe_read_end);
    last_in_left.should_wait = false;
    last_in_left.is_pipe_source = true;

    Vector<Command> commands;
    commands.append(left);
    commands.append(last_in_left);
    commands.append(first_in_right);
    commands.append(right);

    return create<CommandSequenceValue>(move(commands));
}

void Pipe::highlight_in_editor(Line::Editor& editor, Shell& shell, HighlightMetadata metadata)
{
    m_left->highlight_in_editor(editor, shell, metadata);
    m_right->highlight_in_editor(editor, shell, metadata);
}

HitTestResult Pipe::hit_test_position(size_t offset)
{
    if (!position().contains(offset))
        return {};

    auto result = m_left->hit_test_position(offset);
    if (result.matching_node)
        return result;
    return m_right->hit_test_position(offset);
}

Pipe::Pipe(Position position, RefPtr<Node> left, RefPtr<Node> right)
    : Node(move(position))
    , m_left(move(left))
    , m_right(move(right))
{
    if (m_left->is_syntax_error())
        set_is_syntax_error(m_left->syntax_error_node());
    else if (m_right->is_syntax_error())
        set_is_syntax_error(m_right->syntax_error_node());
}

Pipe::~Pipe()
{
}

PathRedirectionNode::PathRedirectionNode(Position position, int fd, RefPtr<Node> path)
    : Node(move(position))
    , m_fd(fd)
    , m_path(move(path))
{
}

void PathRedirectionNode::highlight_in_editor(Line::Editor& editor, Shell& shell, HighlightMetadata metadata)
{
    editor.stylize({ m_position.start_offset, m_position.end_offset }, { Line::Style::Foreground(0x87, 0x9b, 0xcd) }); // 25% Darkened Periwinkle
    metadata.is_first_in_list = false;
    m_path->highlight_in_editor(editor, shell, metadata);
    if (m_path->is_bareword()) {
        auto path_text = m_path->run(nullptr)->resolve_as_list(nullptr);
        ASSERT(path_text.size() == 1);
        // Apply a URL to the path.
        auto& position = m_path->position();
        auto& path = path_text[0];
        if (!path.starts_with('/'))
            path = String::format("%s/%s", shell.cwd.characters(), path.characters());
        auto url = URL::create_with_file_protocol(path);
        url.set_host(shell.hostname);
        editor.stylize({ position.start_offset, position.end_offset }, { Line::Style::Hyperlink(url.to_string()) });
    }
}

HitTestResult PathRedirectionNode::hit_test_position(size_t offset)
{
    if (!position().contains(offset))
        return {};

    auto result = m_path->hit_test_position(offset);
    if (!result.closest_node_with_semantic_meaning)
        result.closest_node_with_semantic_meaning = this;
    return result;
}

Vector<Line::CompletionSuggestion> PathRedirectionNode::complete_for_editor(Shell& shell, size_t offset, const HitTestResult& hit_test_result)
{
    auto matching_node = hit_test_result.matching_node;
    if (!matching_node)
        return {};

    ASSERT(matching_node->is_bareword());
    auto corrected_offset = offset - matching_node->position().start_offset;
    auto* node = static_cast<BarewordLiteral*>(matching_node.ptr());

    if (corrected_offset > node->text().length())
        return {};

    return shell.complete_path("", node->text(), corrected_offset);
}

PathRedirectionNode::~PathRedirectionNode()
{
}

void ReadRedirection::dump(int level) const
{
    Node::dump(level);
    m_path->dump(level + 1);
    print_indented(String::format("To %d", m_fd), level + 1);
}

RefPtr<Value> ReadRedirection::run(RefPtr<Shell> shell)
{
    Command command;
    auto path_segments = m_path->run(shell)->resolve_as_list(shell);
    StringBuilder builder;
    builder.join(" ", path_segments);

    command.redirections.append(*new PathRedirection(builder.to_string(), m_fd, PathRedirection::Read));
    return create<CommandValue>(move(command));
}

ReadRedirection::ReadRedirection(Position position, int fd, RefPtr<Node> path)
    : PathRedirectionNode(move(position), fd, move(path))
{
}

ReadRedirection::~ReadRedirection()
{
}

void ReadWriteRedirection::dump(int level) const
{
    Node::dump(level);
    m_path->dump(level + 1);
    print_indented(String::format("To/From %d", m_fd), level + 1);
}

RefPtr<Value> ReadWriteRedirection::run(RefPtr<Shell> shell)
{
    Command command;
    auto path_segments = m_path->run(shell)->resolve_as_list(shell);
    StringBuilder builder;
    builder.join(" ", path_segments);

    command.redirections.append(*new PathRedirection(builder.to_string(), m_fd, PathRedirection::ReadWrite));
    return create<CommandValue>(move(command));
}

ReadWriteRedirection::ReadWriteRedirection(Position position, int fd, RefPtr<Node> path)
    : PathRedirectionNode(move(position), fd, move(path))
{
}

ReadWriteRedirection::~ReadWriteRedirection()
{
}

void Sequence::dump(int level) const
{
    Node::dump(level);
    m_left->dump(level + 1);
    m_right->dump(level + 1);
}

RefPtr<Value> Sequence::run(RefPtr<Shell> shell)
{
    // If we are to return a job, block on the left one then return the right one.
    if (would_execute()) {
        RefPtr<AST::Node> execute_node = create<AST::Execute>(m_left->position(), m_left);
        auto left_job = execute_node->run(shell);
        ASSERT(left_job->is_job());
        shell->block_on_job(static_cast<JobValue*>(left_job.ptr())->job());

        if (m_right->would_execute())
            return m_right->run(shell);

        execute_node = create<AST::Execute>(m_right->position(), m_right);
        return execute_node->run(shell);
    }
    auto left = m_left->run(shell)->resolve_as_commands(shell);
    // This could happen if a comment is next to a command.
    if (left.size() == 1) {
        auto& command = left.first();
        if (command.argv.is_empty() && command.redirections.is_empty())
            return m_right->run(shell);
    }

    auto right = m_right->run(shell)->resolve_as_commands(shell);

    Vector<Command> commands;
    commands.append(left);
    commands.append(right);

    return create<CommandSequenceValue>(move(commands));
}

void Sequence::highlight_in_editor(Line::Editor& editor, Shell& shell, HighlightMetadata metadata)
{
    m_left->highlight_in_editor(editor, shell, metadata);
    m_right->highlight_in_editor(editor, shell, metadata);
}

HitTestResult Sequence::hit_test_position(size_t offset)
{
    if (!position().contains(offset))
        return {};

    auto result = m_left->hit_test_position(offset);
    if (result.matching_node)
        return result;
    return m_right->hit_test_position(offset);
}

Sequence::Sequence(Position position, RefPtr<Node> left, RefPtr<Node> right)
    : Node(move(position))
    , m_left(move(left))
    , m_right(move(right))
{
    if (m_left->is_syntax_error())
        set_is_syntax_error(m_left->syntax_error_node());
    else if (m_right->is_syntax_error())
        set_is_syntax_error(m_right->syntax_error_node());
}

Sequence::~Sequence()
{
}

void SimpleVariable::dump(int level) const
{
    Node::dump(level);
    print_indented(m_name, level + 1);
}

RefPtr<Value> SimpleVariable::run(RefPtr<Shell>)
{
    return create<SimpleVariableValue>(m_name);
}

void SimpleVariable::highlight_in_editor(Line::Editor& editor, Shell&, HighlightMetadata metadata)
{
    Line::Style style { Line::Style::Foreground(214, 112, 214) };
    if (metadata.is_first_in_list)
        style.unify_with({ Line::Style::Bold });
    editor.stylize({ m_position.start_offset, m_position.end_offset }, move(style));
}

HitTestResult SimpleVariable::hit_test_position(size_t offset)
{
    if (!position().contains(offset))
        return {};

    return { this, this, nullptr };
}

Vector<Line::CompletionSuggestion> SimpleVariable::complete_for_editor(Shell& shell, size_t offset, const HitTestResult& hit_test_result)
{
    auto matching_node = hit_test_result.matching_node;
    if (!matching_node)
        return {};

    if (matching_node != this)
        return {};

    auto corrected_offset = offset - matching_node->position().start_offset - 1;

    if (corrected_offset > m_name.length() + 1)
        return {};

    return shell.complete_variable(m_name, corrected_offset);
}

SimpleVariable::SimpleVariable(Position position, String name)
    : Node(move(position))
    , m_name(move(name))
{
}

SimpleVariable::~SimpleVariable()
{
}

void SpecialVariable::dump(int level) const
{
    Node::dump(level);
    print_indented(String { &m_name, 1 }, level + 1);
}

RefPtr<Value> SpecialVariable::run(RefPtr<Shell>)
{
    return create<SpecialVariableValue>(m_name);
}

void SpecialVariable::highlight_in_editor(Line::Editor& editor, Shell&, HighlightMetadata)
{
    editor.stylize({ m_position.start_offset, m_position.end_offset }, { Line::Style::Foreground(214, 112, 214) });
}

Vector<Line::CompletionSuggestion> SpecialVariable::complete_for_editor(Shell&, size_t, const HitTestResult&)
{
    return {};
}

HitTestResult SpecialVariable::hit_test_position(size_t offset)
{
    if (!position().contains(offset))
        return {};

    return { this, this, nullptr };
}

SpecialVariable::SpecialVariable(Position position, char name)
    : Node(move(position))
    , m_name(name)
{
}

SpecialVariable::~SpecialVariable()
{
}

void Juxtaposition::dump(int level) const
{
    Node::dump(level);
    m_left->dump(level + 1);
    m_right->dump(level + 1);
}

RefPtr<Value> Juxtaposition::run(RefPtr<Shell> shell)
{
    auto left_value = m_left->run(shell)->resolve_without_cast(shell);
    auto right_value = m_right->run(shell)->resolve_without_cast(shell);

    auto left = left_value->resolve_as_list(shell);
    auto right = right_value->resolve_as_list(shell);

    if (left_value->is_string() && right_value->is_string()) {

        ASSERT(left.size() == 1);
        ASSERT(right.size() == 1);

        StringBuilder builder;
        builder.append(left[0]);
        builder.append(right[0]);

        return create<StringValue>(builder.to_string());
    }

    // Otherwise, treat them as lists and create a list product.
    if (left.is_empty() || right.is_empty())
        return create<ListValue>({});

    Vector<String> result;
    result.ensure_capacity(left.size() * right.size());

    StringBuilder builder;
    for (auto& left_element : left) {
        for (auto& right_element : right) {
            builder.append(left_element);
            builder.append(right_element);
            result.append(builder.to_string());
            builder.clear();
        }
    }

    return create<ListValue>(move(result));
}

void Juxtaposition::highlight_in_editor(Line::Editor& editor, Shell& shell, HighlightMetadata metadata)
{
    m_left->highlight_in_editor(editor, shell, metadata);

    // '~/foo/bar' is special, we have to actually resolve the tilde
    // since that resolution is a pure operation, we can just go ahead
    // and do it to get the value :)
    if (m_right->is_bareword() && m_left->is_tilde()) {
        auto tilde_value = m_left->run(shell)->resolve_as_list(shell)[0];
        auto bareword_value = m_right->run(shell)->resolve_as_list(shell)[0];

        StringBuilder path_builder;
        path_builder.append(tilde_value);
        path_builder.append("/");
        path_builder.append(bareword_value);
        auto path = path_builder.to_string();

        if (Core::File::exists(path)) {
            auto realpath = shell.resolve_path(path);
            auto url = URL::create_with_file_protocol(realpath);
            url.set_host(shell.hostname);
            editor.stylize({ m_position.start_offset, m_position.end_offset }, { Line::Style::Hyperlink(url.to_string()) });
        }

    } else {
        m_right->highlight_in_editor(editor, shell, metadata);
    }
}

Vector<Line::CompletionSuggestion> Juxtaposition::complete_for_editor(Shell& shell, size_t offset, const HitTestResult& hit_test_result)
{
    auto matching_node = hit_test_result.matching_node;
    // '~/foo/bar' is special, we have to actually resolve the tilde
    // then complete the bareword with that path prefix.
    if (m_right->is_bareword() && m_left->is_tilde()) {
        auto tilde_value = m_left->run(shell)->resolve_as_list(shell)[0];

        auto corrected_offset = offset - matching_node->position().start_offset;
        auto* node = static_cast<BarewordLiteral*>(matching_node.ptr());

        if (corrected_offset > node->text().length())
            return {};

        auto text = node->text().substring(1, node->text().length() - 1);

        return shell.complete_path(tilde_value, text, corrected_offset - 1);
    }

    return Node::complete_for_editor(shell, offset, hit_test_result);
}

HitTestResult Juxtaposition::hit_test_position(size_t offset)
{
    if (!position().contains(offset))
        return {};

    auto result = m_left->hit_test_position(offset);
    if (!result.closest_node_with_semantic_meaning)
        result.closest_node_with_semantic_meaning = this;
    if (result.matching_node)
        return result;

    result = m_right->hit_test_position(offset);
    if (!result.closest_node_with_semantic_meaning)
        result.closest_node_with_semantic_meaning = this;
    return result;
}

Juxtaposition::Juxtaposition(Position position, RefPtr<Node> left, RefPtr<Node> right)
    : Node(move(position))
    , m_left(move(left))
    , m_right(move(right))
{
    if (m_left->is_syntax_error())
        set_is_syntax_error(m_left->syntax_error_node());
    else if (m_right->is_syntax_error())
        set_is_syntax_error(m_right->syntax_error_node());
}

Juxtaposition::~Juxtaposition()
{
}

void StringLiteral::dump(int level) const
{
    Node::dump(level);
    print_indented(m_text, level + 1);
}

RefPtr<Value> StringLiteral::run(RefPtr<Shell>)
{
    return create<StringValue>(m_text);
}

void StringLiteral::highlight_in_editor(Line::Editor& editor, Shell&, HighlightMetadata metadata)
{
    Line::Style style { Line::Style::Foreground(Line::Style::XtermColor::Yellow) };
    if (metadata.is_first_in_list)
        style.unify_with({ Line::Style::Bold });
    editor.stylize({ m_position.start_offset, m_position.end_offset }, move(style));
}

StringLiteral::StringLiteral(Position position, String text)
    : Node(move(position))
    , m_text(move(text))
{
}

StringLiteral::~StringLiteral()
{
}

void StringPartCompose::dump(int level) const
{
    Node::dump(level);
    m_left->dump(level + 1);
    m_right->dump(level + 1);
}

RefPtr<Value> StringPartCompose::run(RefPtr<Shell> shell)
{
    auto left = m_left->run(shell)->resolve_as_list(shell);
    auto right = m_right->run(shell)->resolve_as_list(shell);

    StringBuilder builder;
    builder.join(" ", left);
    builder.join(" ", right);

    return create<StringValue>(builder.to_string());
}

void StringPartCompose::highlight_in_editor(Line::Editor& editor, Shell& shell, HighlightMetadata metadata)
{
    m_left->highlight_in_editor(editor, shell, metadata);
    m_right->highlight_in_editor(editor, shell, metadata);
}

HitTestResult StringPartCompose::hit_test_position(size_t offset)
{
    if (!position().contains(offset))
        return {};

    auto result = m_left->hit_test_position(offset);
    if (result.matching_node)
        return result;
    return m_right->hit_test_position(offset);
}

StringPartCompose::StringPartCompose(Position position, RefPtr<Node> left, RefPtr<Node> right)
    : Node(move(position))
    , m_left(move(left))
    , m_right(move(right))
{
    if (m_left->is_syntax_error())
        set_is_syntax_error(m_left->syntax_error_node());
    else if (m_right->is_syntax_error())
        set_is_syntax_error(m_right->syntax_error_node());
}

StringPartCompose::~StringPartCompose()
{
}

void SyntaxError::dump(int level) const
{
    Node::dump(level);
}

RefPtr<Value> SyntaxError::run(RefPtr<Shell>)
{
    dbg() << "SYNTAX ERROR AAAA";
    return create<StringValue>("");
}

void SyntaxError::highlight_in_editor(Line::Editor& editor, Shell&, HighlightMetadata)
{
    editor.stylize({ m_position.start_offset, m_position.end_offset }, { Line::Style::Foreground(Line::Style::XtermColor::Red), Line::Style::Bold });
}

SyntaxError::SyntaxError(Position position, String error)
    : Node(move(position))
    , m_syntax_error_text(move(error))
{
    m_is_syntax_error = true;
}

const SyntaxError& SyntaxError::syntax_error_node() const
{
    return *this;
}

SyntaxError::~SyntaxError()
{
}

void Tilde::dump(int level) const
{
    Node::dump(level);
    print_indented(m_username, level + 1);
}

RefPtr<Value> Tilde::run(RefPtr<Shell>)
{
    return create<TildeValue>(m_username);
}

void Tilde::highlight_in_editor(Line::Editor&, Shell&, HighlightMetadata)
{
}

HitTestResult Tilde::hit_test_position(size_t offset)
{
    if (!position().contains(offset))
        return {};

    return { this, this, nullptr };
}

Vector<Line::CompletionSuggestion> Tilde::complete_for_editor(Shell& shell, size_t offset, const HitTestResult& hit_test_result)
{
    auto matching_node = hit_test_result.matching_node;
    if (!matching_node)
        return {};

    if (matching_node != this)
        return {};

    auto corrected_offset = offset - matching_node->position().start_offset - 1;

    if (corrected_offset > m_username.length() + 1)
        return {};

    return shell.complete_user(m_username, corrected_offset);
}

String Tilde::text() const
{
    StringBuilder builder;
    builder.append('~');
    builder.append(m_username);
    return builder.to_string();
}

Tilde::Tilde(Position position, String username)
    : Node(move(position))
    , m_username(move(username))
{
}

Tilde::~Tilde()
{
}

void WriteAppendRedirection::dump(int level) const
{
    Node::dump(level);
    m_path->dump(level + 1);
    print_indented(String::format("From %d", m_fd), level + 1);
}

RefPtr<Value> WriteAppendRedirection::run(RefPtr<Shell> shell)
{
    Command command;
    auto path_segments = m_path->run(shell)->resolve_as_list(shell);
    StringBuilder builder;
    builder.join(" ", path_segments);

    command.redirections.append(*new PathRedirection(builder.to_string(), m_fd, PathRedirection::WriteAppend));
    return create<CommandValue>(move(command));
}

WriteAppendRedirection::WriteAppendRedirection(Position position, int fd, RefPtr<Node> path)
    : PathRedirectionNode(move(position), fd, move(path))
{
}

WriteAppendRedirection::~WriteAppendRedirection()
{
}

void WriteRedirection::dump(int level) const
{
    Node::dump(level);
    m_path->dump(level + 1);
    print_indented(String::format("From %d", m_fd), level + 1);
}

RefPtr<Value> WriteRedirection::run(RefPtr<Shell> shell)
{
    Command command;
    auto path_segments = m_path->run(shell)->resolve_as_list(shell);
    StringBuilder builder;
    builder.join(" ", path_segments);

    command.redirections.append(*new PathRedirection(builder.to_string(), m_fd, PathRedirection::Write));
    return create<CommandValue>(move(command));
}

WriteRedirection::WriteRedirection(Position position, int fd, RefPtr<Node> path)
    : PathRedirectionNode(move(position), fd, move(path))
{
}

WriteRedirection::~WriteRedirection()
{
}

void VariableDeclarations::dump(int level) const
{
    Node::dump(level);
    for (auto& var : m_variables) {
        print_indented("Set", level + 1);
        var.name->dump(level + 2);
        var.value->dump(level + 2);
    }
}

RefPtr<Value> VariableDeclarations::run(RefPtr<Shell> shell)
{
    for (auto& var : m_variables) {
        auto name_value = var.name->run(shell)->resolve_as_list(shell);
        ASSERT(name_value.size() == 1);
        auto name = name_value[0];
        auto value = var.value->run(shell);
        if (value->is_list()) {
            auto parts = value->resolve_as_list(shell);
            shell->set_local_variable(name, adopt(*new ListValue(move(parts))));
        } else if (value->is_command()) {
            shell->set_local_variable(name, value);
        } else {
            auto part = value->resolve_as_list(shell);
            shell->set_local_variable(name, adopt(*new StringValue(part[0])));
        }
    }

    return create<ListValue>({});
}

void VariableDeclarations::highlight_in_editor(Line::Editor& editor, Shell& shell, HighlightMetadata metadata)
{
    metadata.is_first_in_list = false;
    for (auto& var : m_variables) {
        var.name->highlight_in_editor(editor, shell, metadata);
        // Highlight the '='.
        editor.stylize({ var.name->position().end_offset - 1, var.name->position().end_offset }, { Line::Style::Foreground(Line::Style::XtermColor::Blue) });
        var.value->highlight_in_editor(editor, shell, metadata);
    }
}

HitTestResult VariableDeclarations::hit_test_position(size_t offset)
{
    if (!position().contains(offset))
        return {};

    for (auto decl : m_variables) {
        auto result = decl.value->hit_test_position(offset);
        if (result.matching_node)
            return result;
    }

    return { nullptr, nullptr, nullptr };
}

VariableDeclarations::VariableDeclarations(Position position, Vector<Variable> variables)
    : Node(move(position))
    , m_variables(move(variables))
{
    for (auto& decl : m_variables) {
        if (decl.name->is_syntax_error()) {
            set_is_syntax_error(decl.name->syntax_error_node());
            break;
        }
        if (decl.value->is_syntax_error()) {
            set_is_syntax_error(decl.value->syntax_error_node());
            break;
        }
    }
}

VariableDeclarations::~VariableDeclarations()
{
}

Value::~Value()
{
}
Vector<AST::Command> Value::resolve_as_commands(RefPtr<Shell> shell)
{
    Command command;
    command.argv = resolve_as_list(shell);
    return { command };
}

ListValue::ListValue(Vector<String> values)
{
    m_contained_values.ensure_capacity(values.size());
    for (auto& str : values)
        m_contained_values.append(adopt(*new StringValue(move(str))));
}

ListValue::~ListValue()
{
}

Vector<String> ListValue::resolve_as_list(RefPtr<Shell> shell)
{
    Vector<String> values;
    for (auto& value : m_contained_values)
        values.append(value->resolve_as_list(shell));

    return values;
}

CommandValue::~CommandValue()
{
}

CommandSequenceValue::~CommandSequenceValue()
{
}

Vector<String> CommandSequenceValue::resolve_as_list(RefPtr<Shell>)
{
    // TODO: Somehow raise an "error".
    return {};
}

Vector<Command> CommandSequenceValue::resolve_as_commands(RefPtr<Shell>)
{
    return m_contained_values;
}

Vector<String> CommandValue::resolve_as_list(RefPtr<Shell>)
{
    // TODO: Somehow raise an "error".
    return {};
}

Vector<Command> CommandValue::resolve_as_commands(RefPtr<Shell>)
{
    return { m_command };
}

JobValue::~JobValue()
{
}

StringValue::~StringValue()
{
}
Vector<String> StringValue::resolve_as_list(RefPtr<Shell>)
{
    if (is_list()) {
        auto parts = StringView(m_string).split_view(m_split, m_keep_empty);
        Vector<String> result;
        result.ensure_capacity(parts.size());
        for (auto& part : parts)
            result.append(part);
        return result;
    }

    return { m_string };
}

GlobValue::~GlobValue()
{
}
Vector<String> GlobValue::resolve_as_list(RefPtr<Shell> shell)
{
    return shell->expand_globs(m_glob, shell->cwd);
}

SimpleVariableValue::~SimpleVariableValue()
{
}
Vector<String> SimpleVariableValue::resolve_as_list(RefPtr<Shell> shell)
{
    if (auto value = resolve_without_cast(shell); value != this)
        return value->resolve_as_list(shell);

    char* env_value = getenv(m_name.characters());
    if (env_value == nullptr)
        return { "" };

    Vector<String> res;
    String str_env_value = String(env_value);
    const auto& split_text = str_env_value.split_view(' ');
    for (auto& part : split_text)
        res.append(part);
    return res;
}

RefPtr<Value> SimpleVariableValue::resolve_without_cast(RefPtr<Shell> shell)
{

    if (auto value = shell->lookup_local_variable(m_name))
        return value;

    return this;
}

SpecialVariableValue::~SpecialVariableValue()
{
}
Vector<String> SpecialVariableValue::resolve_as_list(RefPtr<Shell> shell)
{
    switch (m_name) {
    case '?':
        return { String::number(shell->last_return_code) };
    case '$':
        return { String::number(getpid()) };
    default:
        return { "" };
    }
}

TildeValue::~TildeValue()
{
}
Vector<String> TildeValue::resolve_as_list(RefPtr<Shell> shell)
{
    StringBuilder builder;
    builder.append("~");
    builder.append(m_username);
    return { shell->expand_tilde(builder.to_string()) };
}

Result<RefPtr<Rewiring>, String> CloseRedirection::apply() const
{
    return static_cast<RefPtr<Rewiring>>((adopt(*new Rewiring(fd, fd, Rewiring::Close::ImmediatelyCloseDestination))));
}

CloseRedirection::~CloseRedirection()
{
}

Result<RefPtr<Rewiring>, String> PathRedirection::apply() const
{
    auto check_fd_and_return = [my_fd = this->fd](int fd, const String& path) -> Result<RefPtr<Rewiring>, String> {
        if (fd < 0) {
            String error = strerror(errno);
            dbg() << "open() failed for '" << path << "' with " << error;
            return error;
        }
        return static_cast<RefPtr<Rewiring>>((adopt(*new Rewiring(my_fd, fd, Rewiring::Close::Destination))));
    };
    switch (direction) {
    case AST::PathRedirection::WriteAppend:
        return check_fd_and_return(open(path.characters(), O_WRONLY | O_CREAT | O_APPEND, 0666), path);

    case AST::PathRedirection::Write:
        return check_fd_and_return(open(path.characters(), O_WRONLY | O_CREAT | O_TRUNC, 0666), path);

    case AST::PathRedirection::Read:
        return check_fd_and_return(open(path.characters(), O_RDONLY), path);

    case AST::PathRedirection::ReadWrite:
        return check_fd_and_return(open(path.characters(), O_RDWR | O_CREAT, 0666), path);
    }

    ASSERT_NOT_REACHED();
}

PathRedirection::~PathRedirection()
{
}

FdRedirection::~FdRedirection()
{
}

Redirection::~Redirection()
{
}

}
