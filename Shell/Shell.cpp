/*
 * Copyright (c) 2020, The SerenityOS developers.
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

#include "Shell.h"
#include "Execution.h"
#include <AK/Function.h>
#include <AK/LexicalPath.h>
#include <AK/ScopeGuard.h>
#include <AK/StringBuilder.h>
#include <LibCore/ArgsParser.h>
#include <LibCore/DirIterator.h>
#include <LibCore/ElapsedTimer.h>
#include <LibCore/Event.h>
#include <LibCore/EventLoop.h>
#include <LibCore/File.h>
#include <LibLine/Editor.h>
#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <termios.h>
#include <unistd.h>

static bool s_disable_hyperlinks = false;
extern RefPtr<Line::Editor> editor;

//#define SH_DEBUG

void Shell::print_path(const String& path)
{
    if (s_disable_hyperlinks) {
        printf("%s", path.characters());
        return;
    }
    printf("\033]8;;file://%s%s\033\\%s\033]8;;\033\\", hostname, path.characters(), path.characters());
}

String Shell::prompt() const
{
    auto build_prompt = [&]() -> String {
        auto* ps1 = getenv("PROMPT");
        if (!ps1) {
            if (uid == 0)
                return "# ";

            StringBuilder builder;
            builder.appendf("\033]0;%s@%s:%s\007", username.characters(), hostname, cwd.characters());
            builder.appendf("\033[31;1m%s\033[0m@\033[37;1m%s\033[0m:\033[32;1m%s\033[0m$> ", username.characters(), hostname, cwd.characters());
            return builder.to_string();
        }

        StringBuilder builder;
        for (char* ptr = ps1; *ptr; ++ptr) {
            if (*ptr == '\\') {
                ++ptr;
                if (!*ptr)
                    break;
                switch (*ptr) {
                case 'X':
                    builder.append("\033]0;");
                    break;
                case 'a':
                    builder.append(0x07);
                    break;
                case 'e':
                    builder.append(0x1b);
                    break;
                case 'u':
                    builder.append(username);
                    break;
                case 'h':
                    builder.append(hostname);
                    break;
                case 'w': {
                    String home_path = getenv("HOME");
                    if (cwd.starts_with(home_path)) {
                        builder.append('~');
                        builder.append(cwd.substring_view(home_path.length(), cwd.length() - home_path.length()));
                    } else {
                        builder.append(cwd);
                    }
                    break;
                }
                case 'p':
                    builder.append(uid == 0 ? '#' : '$');
                    break;
                }
                continue;
            }
            builder.append(*ptr);
        }
        return builder.to_string();
    };

    return build_prompt();
}

String Shell::expand_tilde(const String& expression)
{
    ASSERT(expression.starts_with('~'));

    StringBuilder login_name;
    size_t first_slash_index = expression.length();
    for (size_t i = 1; i < expression.length(); ++i) {
        if (expression[i] == '/') {
            first_slash_index = i;
            break;
        }
        login_name.append(expression[i]);
    }

    StringBuilder path;
    for (size_t i = first_slash_index; i < expression.length(); ++i)
        path.append(expression[i]);

    if (login_name.is_empty()) {
        const char* home = getenv("HOME");
        if (!home) {
            auto passwd = getpwuid(getuid());
            ASSERT(passwd && passwd->pw_dir);
            return String::format("%s/%s", passwd->pw_dir, path.to_string().characters());
        }
        return String::format("%s/%s", home, path.to_string().characters());
    }

    auto passwd = getpwnam(login_name.to_string().characters());
    if (!passwd)
        return expression;
    ASSERT(passwd->pw_dir);

    return String::format("%s/%s", passwd->pw_dir, path.to_string().characters());
}

bool Shell::is_glob(const StringView& s)
{
    for (size_t i = 0; i < s.length(); i++) {
        char c = s.characters_without_null_termination()[i];
        if (c == '*' || c == '?')
            return true;
    }
    return false;
}

Vector<StringView> Shell::split_path(const StringView& path)
{
    Vector<StringView> parts;

    size_t substart = 0;
    for (size_t i = 0; i < path.length(); i++) {
        char ch = path[i];
        if (ch != '/')
            continue;
        size_t sublen = i - substart;
        if (sublen != 0)
            parts.append(path.substring_view(substart, sublen));
        substart = i + 1;
    }

    size_t taillen = path.length() - substart;
    if (taillen != 0)
        parts.append(path.substring_view(substart, taillen));

    return parts;
}

Vector<String> Shell::expand_globs(const StringView& path, StringView base)
{
    if (path.starts_with('/'))
        base = "/";
    auto parts = split_path(path);
    String base_string = base;
    struct stat statbuf;
    if (lstat(base_string.characters(), &statbuf) < 0) {
        perror("lstat");
        return {};
    }

    StringBuilder resolved_base_path_builder;
    resolved_base_path_builder.append(Core::File::real_path_for(base));
    if (S_ISDIR(statbuf.st_mode))
        resolved_base_path_builder.append('/');

    auto resolved_base = resolved_base_path_builder.string_view();

    auto results = expand_globs(move(parts), resolved_base);

    for (auto& entry : results) {
        entry = entry.substring(resolved_base.length(), entry.length() - resolved_base.length());
        if (entry.is_empty())
            entry = ".";
    }

    // Make the output predictable and nice.
    quick_sort(results);

    return results;
}

Vector<String> Shell::expand_globs(Vector<StringView> path_segments, const StringView& base)
{
    if (path_segments.is_empty()) {
        String base_str = base;
        if (access(base_str.characters(), F_OK) == 0)
            return { move(base_str) };
        return {};
    }

    auto first_segment = path_segments.take_first();
    if (is_glob(first_segment)) {
        Vector<String> result;

        Core::DirIterator di(base, Core::DirIterator::SkipParentAndBaseDir);
        if (di.has_error())
            return {};

        while (di.has_next()) {
            String path = di.next_path();

            // Dotfiles have to be explicitly requested
            if (path[0] == '.' && first_segment[0] != '.')
                continue;

            if (path.matches(first_segment, CaseSensitivity::CaseSensitive)) {
                StringBuilder builder;
                builder.append(base);
                if (!base.ends_with('/'))
                    builder.append('/');
                builder.append(path);
                result.append(expand_globs(path_segments, builder.string_view()));
            }
        }

        return result;
    } else {
        StringBuilder builder;
        builder.append(base);
        if (!base.ends_with('/'))
            builder.append('/');
        builder.append(first_segment);

        return expand_globs(move(path_segments), builder.string_view());
    }
}

Vector<AST::Command> Shell::expand_aliases(Vector<AST::Command> initial_commands)
{
    Vector<AST::Command> commands;

    Function<void(AST::Command&)> resolve_aliases_and_append = [&](auto& command) {
        if (!command.argv.is_empty()) {
            auto alias = resolve_alias(command.argv[0]);
            if (!alias.is_null()) {
                auto argv0 = command.argv.take_first();
                auto subcommand_ast = Parser { alias }.parse();
                if (subcommand_ast) {
                    while (subcommand_ast->is_execute()) {
                        auto* ast = static_cast<AST::Execute*>(subcommand_ast.ptr());
                        subcommand_ast = ast->command();
                    }
                    AST::Node& substitute = *new AST::Join(subcommand_ast->position(), subcommand_ast, *new AST::CommandLiteral(subcommand_ast->position(), command));
                    for (auto& subst_command : substitute.run(*this)->resolve_as_commands(*this)) {
                        if (!subst_command.argv.is_empty() && subst_command.argv.first() == argv0) // Disallow an alias resolving to itself.
                            commands.append(subst_command);
                        else
                            resolve_aliases_and_append(subst_command);
                    }
                } else {
                    commands.append(command);
                }
            } else {
                commands.append(command);
            }
        } else {
            commands.append(command);
        }
    };

    for (auto& command : initial_commands)
        resolve_aliases_and_append(command);

    return commands;
}

String Shell::resolve_path(String path) const
{
    if (!path.starts_with('/'))
        path = String::format("%s/%s", cwd.characters(), path.characters());

    return Core::File::real_path_for(path);
}

RefPtr<AST::Value> Shell::lookup_local_variable(const String& name)
{
    auto value = m_local_variables.get(name).value_or(nullptr);
    return value;
}

String Shell::local_variable_or(const String& name, const String& replacement)
{
    auto value = lookup_local_variable(name);
    if (value) {
        StringBuilder builder;
        builder.join(" ", value->resolve_as_list(*this));
        return builder.to_string();
    }
    return replacement;
}

void Shell::set_local_variable(const String& name, RefPtr<AST::Value> value)
{
    m_local_variables.set(name, move(value));
}

void Shell::unset_local_variable(const String& name)
{
    m_local_variables.remove(name);
}

String Shell::resolve_alias(const String& name) const
{
    return m_aliases.get(name).value_or({});
}

int Shell::run_command(const StringView& cmd)
{
    if (cmd.is_empty())
        return 0;

    auto command = Parser(cmd).parse();

    if (!command)
        return 0;

    if (command->is_syntax_error()) {
        auto& error_node = command->syntax_error_node();
        auto& position = error_node.position();
        fprintf(stderr, "Shell: Syntax error in command: %s\n", error_node.error_text().characters());
        fprintf(stderr, "Around '%.*s'\n", (int)min(position.end_offset - position.start_offset, (size_t)10), cmd.characters_without_null_termination() + position.start_offset);
        return 1;
    }

#ifdef SH_DEBUG
    dbg() << "Command follows";
    command->dump(0);
#endif

    tcgetattr(0, &termios);

    auto result = command->run(*this);
    if (result->is_job()) {
        auto job_result = static_cast<AST::JobValue*>(result.ptr());
        auto job = job_result->job();
        if (!job)
            last_return_code = 0;
        else if (job->exited())
            last_return_code = job->exit_code();
    }

    return last_return_code;
}

RefPtr<Job> Shell::run_command(const AST::Command& command)
{
    FileDescriptionCollector fds;

    if (options.verbose) {
        fprintf(stderr, "+ ");
        for (auto& arg : command.argv)
            fprintf(stderr, "%s ", escape_token(arg).characters());
        fprintf(stderr, "\n");
        fflush(stderr);
    }

    // Resolve redirections.
    NonnullRefPtrVector<AST::Rewiring> rewirings;
    for (auto& redirection : command.redirections) {
        auto rewiring_result = redirection->apply();
        if (rewiring_result.is_error()) {
            if (!rewiring_result.error().is_empty())
                fprintf(stderr, "error: %s\n", rewiring_result.error().characters());
            continue;
        }
        auto& rewiring = rewiring_result.value();

        if (rewiring->fd_action != AST::Rewiring::Close::ImmediatelyCloseDestination)
            rewirings.append(*rewiring);

        if (rewiring->fd_action == AST::Rewiring::Close::Source) {
            fds.add(rewiring->source_fd);
        } else if (rewiring->fd_action == AST::Rewiring::Close::Destination) {
            if (rewiring->dest_fd != -1)
                fds.add(rewiring->dest_fd);
        } else if (rewiring->fd_action == AST::Rewiring::Close::ImmediatelyCloseDestination) {
            fds.add(rewiring->dest_fd);
        } else if (rewiring->fd_action == AST::Rewiring::Close::RefreshDestination) {
            ASSERT(rewiring->other_pipe_end);

            int pipe_fd[2];
            int rc = pipe(pipe_fd);
            if (rc < 0) {
                perror("pipe(RedirRefresh)");
                return nullptr;
            }
            rewiring->dest_fd = pipe_fd[1];
            rewiring->other_pipe_end->dest_fd = pipe_fd[0]; // This fd will be added to the collection on one of the next iterations.
            fds.add(pipe_fd[1]);
        }
    }

    // If the command is empty, do all the rewirings in the current process and return.
    // This allows the user to mess with the shell internals, but is apparently useful?
    // We'll just allow the users to shoot themselves until they get tired of doing so.
    if (command.argv.is_empty()) {
        for (auto& rewiring : rewirings) {
#ifdef SH_DEBUG
            dbgprintf("in %d, dup2(%d, %d)\n", getpid(), rewiring.dest_fd, rewiring.source_fd);
#endif
            int rc = dup2(rewiring.dest_fd, rewiring.source_fd);
            if (rc < 0) {
                perror("dup2(run)");
                return nullptr;
            }
        }

        fds.collect();
        return nullptr;
    }

    Vector<const char*> argv;
    Vector<String> copy_argv = command.argv;
    argv.ensure_capacity(command.argv.size() + 1);

    for (auto& arg : copy_argv)
        argv.append(arg.characters());

    argv.append(nullptr);

    int retval = 0;
    if (run_builtin(argv.size() - 1, argv.data(), retval))
        return nullptr;

    pid_t child = fork();
    if (child < 0) {
        perror("fork");
        return nullptr;
    }
    if (child == 0) {
        setpgid(0, 0);
        tcsetattr(0, TCSANOW, &default_termios);
        for (auto& rewiring : rewirings) {
#ifdef SH_DEBUG
            dbgprintf("in %s<%d>, dup2(%d, %d)\n", argv[0], getpid(), rewiring.dest_fd, rewiring.source_fd);
#endif
            int rc = dup2(rewiring.dest_fd, rewiring.source_fd);
            if (rc < 0) {
                perror("dup2(run)");
                return nullptr;
            }
        }

        fds.collect();

        int rc = execvp(argv[0], const_cast<char* const*>(argv.data()));
        if (rc < 0) {
            if (errno == ENOENT) {
                int shebang_fd = open(argv[0], O_RDONLY);
                auto close_argv = ScopeGuard([shebang_fd]() { if (shebang_fd >= 0)  close(shebang_fd); });
                char shebang[256] {};
                ssize_t num_read = -1;
                if ((shebang_fd >= 0) && ((num_read = read(shebang_fd, shebang, sizeof(shebang))) >= 2) && (StringView(shebang).starts_with("#!"))) {
                    StringView shebang_path_view(&shebang[2], num_read - 2);
                    Optional<size_t> newline_pos = shebang_path_view.find_first_of("\n\r");
                    shebang[newline_pos.has_value() ? (newline_pos.value() + 2) : num_read] = '\0';
                    fprintf(stderr, "%s: Invalid interpreter \"%s\": %s\n", argv[0], &shebang[2], strerror(ENOENT));
                } else
                    fprintf(stderr, "%s: Command not found.\n", argv[0]);
            } else {
                int saved_errno = errno;
                struct stat st;
                if (stat(argv[0], &st) == 0 && S_ISDIR(st.st_mode)) {
                    fprintf(stderr, "Shell: %s: Is a directory\n", argv[0]);
                    _exit(126);
                }
                fprintf(stderr, "execvp(%s): %s\n", argv[0], strerror(saved_errno));
            }
            _exit(126);
        }
        ASSERT_NOT_REACHED();
    }

    StringBuilder cmd;
    cmd.join(" ", command.argv);

    auto job = adopt(*new Job(child, (unsigned)child, cmd.build(), find_last_job_id() + 1));
    jobs.set((u64)child, job);

    job->on_exit = [](auto job) {
        if (!job->exited())
            return;
        if (job->is_running_in_background())
            fprintf(stderr, "Shell: Job %d(%s) exited\n", job->pid(), job->cmd().characters());
        job->disown();
    };

    fds.collect();

    return *job;
}

Vector<RefPtr<Job>> Shell::run_commands(Vector<AST::Command>& commands)
{
    Vector<RefPtr<Job>> jobs_to_wait_for;

    for (auto& command : commands) {
#ifdef SH_DEBUG
        dbg() << "Command";
        for (auto& arg : command.argv)
            dbg() << "argv: " << arg;
        for (auto& redir : command.redirections) {
            if (redir->is_path_redirection()) {
                auto path_redir = (const AST::PathRedirection*)redir.ptr();
                dbg() << "redir path " << (int)path_redir->direction << " " << path_redir->path << " <-> " << path_redir->fd;
            } else if (redir->is_fd_redirection()) {
                auto fd_redir = (const AST::FdRedirection*)redir.ptr();
                dbg() << "redir fd " << fd_redir->source_fd << " -> " << fd_redir->dest_fd;
            } else if (redir->is_close_redirection()) {
                auto close_redir = (const AST::CloseRedirection*)redir.ptr();
                dbg() << "close fd " << close_redir->fd;
            } else {
                ASSERT_NOT_REACHED();
            }
        }
#endif
        auto job = run_command(command);
        if (!job)
            continue;

        if (command.should_wait) {
            block_on_job(job);
            if (!job->is_suspended())
                jobs_to_wait_for.append(job);
        } else {
            if (command.is_pipe_source) {
                jobs_to_wait_for.append(job);
            } else if (command.should_notify_if_in_background) {
                job->set_running_in_background(true);
                restore_stdin();
            }
        }
    }

    return jobs_to_wait_for;
}

bool Shell::run_file(const String& filename, bool explicitly_invoked)
{
    auto file_result = Core::File::open(filename, Core::File::ReadOnly);
    if (file_result.is_error()) {
        if (explicitly_invoked)
            fprintf(stderr, "Failed to open %s: %s\n", filename.characters(), file_result.error().characters());
        else
            dbg() << "open() failed for '" << filename << "' with " << file_result.error();
        return false;
    }
    auto file = file_result.value();
    auto data = file->read_all();
    run_command(data);
    return true;
}
void Shell::restore_stdin()
{
    tcsetattr(0, TCSANOW, &termios);
}

void Shell::block_on_job(RefPtr<Job> job)
{
    TemporaryChange<const Job*> current_job { m_current_job, job.ptr() };

    if (!job)
        return;

    Core::EventLoop loop;
    job->on_exit = [&, old_exit = move(job->on_exit)](auto job) {
        if (old_exit)
            old_exit(job);
        loop.quit(0);
    };
    if (job->exited()) {
        restore_stdin();
        return;
    }

    loop.exec();

    restore_stdin();
}

String Shell::get_history_path()
{
    StringBuilder builder;
    builder.append(home);
    builder.append("/.history");
    return builder.to_string();
}

void Shell::load_history()
{
    auto history_file = Core::File::construct(get_history_path());
    if (!history_file->open(Core::IODevice::ReadOnly))
        return;
    while (history_file->can_read_line()) {
        auto b = history_file->read_line(1024);
        // skip the newline and terminating bytes
        editor->add_to_history(String(reinterpret_cast<const char*>(b.data()), b.size() - 2));
    }
}

void Shell::save_history()
{
    auto file_or_error = Core::File::open(get_history_path(), Core::IODevice::WriteOnly, 0600);
    if (file_or_error.is_error())
        return;
    auto& file = *file_or_error.value();
    for (const auto& line : editor->history()) {
        file.write(line);
        file.write("\n");
    }
}

String Shell::escape_token(const String& token)
{
    StringBuilder builder;

    for (auto c : token) {
        switch (c) {
        case '\'':
        case '"':
        case '$':
        case '|':
        case '>':
        case '<':
        case '&':
        case '\\':
        case ' ':
            builder.append('\\');
            break;
        default:
            break;
        }
        builder.append(c);
    }

    return builder.build();
}

String Shell::unescape_token(const String& token)
{
    StringBuilder builder;

    enum {
        Free,
        Escaped
    } state { Free };

    for (auto c : token) {
        switch (state) {
        case Escaped:
            builder.append(c);
            state = Free;
            break;
        case Free:
            if (c == '\\')
                state = Escaped;
            else
                builder.append(c);
            break;
        }
    }

    if (state == Escaped)
        builder.append('\\');

    return builder.build();
}

void Shell::cache_path()
{
    if (!cached_path.is_empty())
        cached_path.clear_with_capacity();

    // Add shell builtins to the cache.
    for (const auto& builtin_name : builtin_names)
        cached_path.append(escape_token(builtin_name));

    // Add aliases to the cache.
    for (const auto& alias : m_aliases) {
        auto name = escape_token(alias.key);
        if (cached_path.contains_slow(name))
            continue;
        cached_path.append(name);
    }

    String path = getenv("PATH");
    if (!path.is_empty()) {
        auto directories = path.split(':');
        for (const auto& directory : directories) {
            Core::DirIterator programs(directory.characters(), Core::DirIterator::SkipDots);
            while (programs.has_next()) {
                auto program = programs.next_path();
                String program_path = String::format("%s/%s", directory.characters(), program.characters());
                auto escaped_name = escape_token(program);
                if (cached_path.contains_slow(escaped_name))
                    continue;
                if (access(program_path.characters(), X_OK) == 0)
                    cached_path.append(escaped_name);
            }
        }
    }

    quick_sort(cached_path);
}

void Shell::highlight(Line::Editor& editor) const
{
    auto line = editor.line();
    Parser parser(line);
    auto ast = parser.parse();
    if (!ast)
        return;
    ast->highlight_in_editor(editor, const_cast<Shell&>(*this));
}

Vector<Line::CompletionSuggestion> Shell::complete(const Line::Editor& editor)
{
    auto line = editor.line(editor.cursor());

    Parser parser(line);

    auto ast = parser.parse();

    if (!ast)
        return {};

    return ast->complete_for_editor(*this, line.length());
}

Vector<Line::CompletionSuggestion> Shell::complete_path(const String& base, const String& part, size_t offset)
{
    auto token = offset ? part.substring_view(0, offset) : "";
    StringView original_token = token;
    String path;

    ssize_t last_slash = token.length() - 1;
    while (last_slash >= 0 && token[last_slash] != '/')
        --last_slash;

    StringBuilder path_builder;
    auto init_slash_part = token.substring_view(0, last_slash + 1);
    auto last_slash_part = token.substring_view(last_slash + 1, token.length() - last_slash - 1);

    // Depending on the base, we will have to prepend cwd.
    if (base.is_empty()) {
        // '' /foo -> absolute
        // '' foo -> relative
        if (!token.starts_with('/'))
            path_builder.append(cwd);
        path_builder.append('/');
        path_builder.append(init_slash_part);
    } else {
        // /foo * -> absolute
        // foo * -> relative
        if (!base.starts_with('/'))
            path_builder.append(cwd);
        path_builder.append('/');
        path_builder.append(base);
        path_builder.append('/');
        path_builder.append(init_slash_part);
    }
    path = path_builder.build();
    token = last_slash_part;

    // the invariant part of the token is actually just the last segment
    // e. in `cd /foo/bar', 'bar' is the invariant
    //      since we are not suggesting anything starting with
    //      `/foo/', but rather just `bar...'
    auto token_length = escape_token(token).length();
    editor->suggest(token_length, original_token.length() - token_length);

    // only suggest dot-files if path starts with a dot
    Core::DirIterator files(path,
        token.starts_with('.') ? Core::DirIterator::SkipParentAndBaseDir : Core::DirIterator::SkipDots);

    Vector<Line::CompletionSuggestion> suggestions;

    while (files.has_next()) {
        auto file = files.next_path();
        if (file.starts_with(token)) {
            struct stat program_status;
            String file_path = String::format("%s/%s", path.characters(), file.characters());
            int stat_error = stat(file_path.characters(), &program_status);
            if (!stat_error) {
                if (S_ISDIR(program_status.st_mode)) {
                    suggestions.append({ escape_token(file), "/" });
                } else {
                    suggestions.append({ escape_token(file), " " });
                }
            }
        }
    }

    return suggestions;
}

Vector<Line::CompletionSuggestion> Shell::complete_program_name(const String& name, size_t offset)
{
    auto match = binary_search(cached_path.data(), cached_path.size(), name, [](const String& name, const String& program) -> int {
        return strncmp(name.characters(), program.characters(), name.length());
    });

    if (!match)
        return complete_path("", name, offset);

    String completion = *match;
    editor->suggest(escape_token(name).length(), 0);

    // Now that we have a program name starting with our token, we look at
    // other program names starting with our token and cut off any mismatching
    // characters.

    Vector<Line::CompletionSuggestion> suggestions;

    int index = match - cached_path.data();
    for (int i = index - 1; i >= 0 && cached_path[i].starts_with(name); --i) {
        suggestions.append({ cached_path[i], " " });
    }
    for (size_t i = index + 1; i < cached_path.size() && cached_path[i].starts_with(name); ++i) {
        suggestions.append({ cached_path[i], " " });
    }
    suggestions.append({ cached_path[index], " " });

    return suggestions;
}

Vector<Line::CompletionSuggestion> Shell::complete_variable(const String& name, size_t offset)
{
    Vector<Line::CompletionSuggestion> suggestions;
    auto pattern = offset ? name.substring_view(0, offset) : "";

    editor->suggest(offset);

    // Look at local variables.
    for (auto& variable : m_local_variables) {
        if (variable.key.starts_with(pattern))
            suggestions.append(variable.key);
    }

    // Look at the environment.
    for (auto i = 0; environ[i]; ++i) {
        auto entry = StringView { environ[i] };
        if (entry.starts_with(pattern)) {
            auto parts = entry.split_view('=');
            if (parts.is_empty() || parts.first().is_empty())
                continue;
            String name = parts.first();
            if (suggestions.contains_slow(name))
                continue;
            suggestions.append(move(name));
        }
    }

    return suggestions;
}

Vector<Line::CompletionSuggestion> Shell::complete_user(const String& name, size_t offset)
{
    Vector<Line::CompletionSuggestion> suggestions;
    auto pattern = offset ? name.substring_view(0, offset) : "";

    editor->suggest(offset);

    Core::DirIterator di("/home", Core::DirIterator::SkipParentAndBaseDir);

    if (di.has_error())
        return suggestions;

    while (di.has_next()) {
        String name = di.next_path();
        if (name.starts_with(pattern))
            suggestions.append(name);
    }

    return suggestions;
}

Vector<Line::CompletionSuggestion> Shell::complete_option(const String& program_name, const String& option, size_t offset)
{
    size_t start = 0;
    while (start < option.length() && option[start] == '-' && start < 2)
        ++start;
    auto option_pattern = offset > start ? option.substring_view(start, offset - start) : "";
    editor->suggest(offset);

    Vector<Line::CompletionSuggestion> suggestions;

    dbg() << "Shell::complete_option(" << program_name << ", " << option_pattern << ")";

    // FIXME: Figure out how to do this stuff.
    if (has_builtin(program_name)) {
        // Complete builtins.
        if (program_name == "setopt") {
            bool negate = false;
            if (option_pattern.starts_with("no_")) {
                negate = true;
                option_pattern = option_pattern.substring_view(3, option_pattern.length() - 3);
            }
            auto maybe_negate = [&](const StringView& view) {
                static StringBuilder builder;
                builder.clear();
                builder.append("--");
                if (negate)
                    builder.append("no_");
                builder.append(view);
                return builder.to_string();
            };
#define __ENUMERATE_SHELL_OPTION(name, d_, descr_)        \
    if (StringView { #name }.starts_with(option_pattern)) \
        suggestions.append(maybe_negate(#name));

            ENUMERATE_SHELL_OPTIONS();
#undef __ENUMERATE_SHELL_OPTION
            return suggestions;
        }
    }
    return suggestions;
}

bool Shell::read_single_line()
{
    restore_stdin();
    auto line_result = editor->get_line(prompt());

    if (line_result.is_error()) {
        if (line_result.error() == Line::Editor::Error::Eof || line_result.error() == Line::Editor::Error::Empty) {
            // Pretend the user tried to execute builtin_exit()
            m_complete_line_builder.clear();
            run_command("exit");
            return read_single_line();
        } else {
            m_complete_line_builder.clear();
            Core::EventLoop::current().quit(1);
            return false;
        }
    }

    auto& line = line_result.value();

    if (line.is_empty())
        return true;

    if (!m_complete_line_builder.is_empty())
        m_complete_line_builder.append("\n");
    m_complete_line_builder.append(line);

    run_command(m_complete_line_builder.string_view());

    editor->add_to_history(m_complete_line_builder.build());
    m_complete_line_builder.clear();
    return true;
}

void Shell::custom_event(Core::CustomEvent& event)
{
    if (event.custom_type() == ReadLine) {
        if (read_single_line())
            Core::EventLoop::current().post_event(*this, make<Core::CustomEvent>(ShellEventType::ReadLine));
        return;
    }

    event.ignore();
}

Shell::Shell()
{
    uid = getuid();
    tcsetpgrp(0, getpgrp());
    m_pid = getpid();

    int rc = gethostname(hostname, Shell::HostNameSize);
    if (rc < 0)
        perror("gethostname");
    rc = ttyname_r(0, ttyname, Shell::TTYNameSize);
    if (rc < 0)
        perror("ttyname_r");

    {
        auto* cwd = getcwd(nullptr, 0);
        this->cwd = cwd;
        setenv("PWD", cwd, 1);
        free(cwd);
    }

    {
        auto* pw = getpwuid(getuid());
        if (pw) {
            username = pw->pw_name;
            home = pw->pw_dir;
            setenv("HOME", pw->pw_dir, 1);
        }
        endpwent();
    }

    directory_stack.append(cwd);
    load_history();
    cache_path();
}

Shell::~Shell()
{
    stop_all_jobs();
    save_history();
}

void Shell::stop_all_jobs()
{
    if (!jobs.is_empty()) {
        printf("Killing active jobs\n");
        for (auto& entry : jobs) {
            if (!entry.value->is_running_in_background()) {
#ifdef SH_DEBUG
                dbg() << "Job " << entry.value->pid() << " is not running in background";
#endif
                kill_job(entry.value, SIGCONT);
            }

            kill_job(entry.value, SIGHUP);
        }

        usleep(10000); // Wait for a bit before killing the job

        for (auto& entry : jobs) {
#ifdef SH_DEBUG
            dbg() << "Actively killing " << entry.value->pid() << "(" << entry.value->cmd() << ")";
#endif
            if (killpg(entry.value->pgid(), SIGKILL) < 0) {
                if (errno == ESRCH)
                    continue; // The process has exited all by itself.
                perror("killpg(KILL)");
            }
        }
    }
}

u64 Shell::find_last_job_id() const
{
    u64 job_id = 0;
    for (auto& entry : jobs) {
        if (entry.value->job_id() > job_id)
            job_id = entry.value->job_id();
    }
    return job_id;
}

const Job* Shell::find_job(u64 id)
{
    for (auto& entry : jobs) {
        if (entry.value->job_id() == id)
            return entry.value;
    }
    return nullptr;
}

void Shell::kill_job(const Job* job, int sig)
{
    if (!job)
        return;

    if (killpg(job->pgid(), sig) < 0)
        perror("killpg(job)");
}

void Shell::save_to(JsonObject& object)
{
    Core::Object::save_to(object);
    object.set("working_directory", cwd);
    object.set("username", username);
    object.set("user_home_path", home);
    object.set("user_id", uid);
    object.set("directory_stack_size", directory_stack.size());
    object.set("cd_history_size", cd_history.size());

    // Jobs.
    JsonArray job_objects;
    for (auto& job_entry : jobs) {
        JsonObject job_object;
        job_object.set("pid", job_entry.value->pid());
        job_object.set("pgid", job_entry.value->pgid());
        job_object.set("running_time", job_entry.value->timer().elapsed());
        job_object.set("command", job_entry.value->cmd());
        job_object.set("is_running_in_background", job_entry.value->is_running_in_background());
        job_objects.append(move(job_object));
    }
    object.set("jobs", move(job_objects));
}
