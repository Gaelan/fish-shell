/** \file builtin_commandline.c Functions defining the commandline builtin

Functions used for implementing the commandline builtin.

*/
#include "config.h"

#include <stdlib.h>
#include <stdio.h>
#include <wchar.h>
#include <wctype.h>
#include <sys/types.h>
#include <termios.h>
#include <signal.h>

#include "fallback.h"
#include "util.h"

#include "wutil.h"
#include "builtin.h"
#include "common.h"
#include "wgetopt.h"
#include "reader.h"
#include "proc.h"
#include "parser.h"
#include "tokenizer.h"
#include "input_common.h"
#include "input.h"
#include "iothread.h"

#include "parse_util.h"

/**
   Which part of the comandbuffer are we operating on
*/
enum
{
    STRING_MODE=1, /**< Operate on entire buffer */
    JOB_MODE, /**< Operate on job under cursor */
    PROCESS_MODE, /**< Operate on process under cursor */
    TOKEN_MODE /**< Operate on token under cursor */
}
;

/**
   For text insertion, how should it be done
*/
enum
{
    REPLACE_MODE=1, /**< Replace current text */
    INSERT_MODE, /**< Insert at cursor position */
    APPEND_MODE /**< Insert at end of current token/command/buffer */
}
;

builtin_commandline_scoped_transient_t::builtin_commandline_scoped_transient_t(parser_t *p, const wcstring &cmd) : parser(p)
{
    assert(p != NULL);
    p->assert_is_this_thread();
    p->push_substituted_commandline(cmd);
}

builtin_commandline_scoped_transient_t::~builtin_commandline_scoped_transient_t()
{
    parser->assert_is_this_thread();
    parser->pop_substituted_commandline();
}

static void apply_new_commandline_and_delete(editable_line_t *buffer)
{
    ASSERT_IS_MAIN_THREAD();
    assert(buffer != NULL);
    reader_set_buffer(buffer->text, buffer->position);
    delete buffer;
}

static void apply_new_commandline(const editable_line_t &buffer)
{
    // Hackish
    if (is_main_thread())
    {
        reader_set_buffer(buffer.text, buffer.position);
    }
    else
    {
        iothread_enqueue_to_main(apply_new_commandline_and_delete, new editable_line_t(buffer));
    }
}

/**
   Replace/append/insert the selection with/at/after the specified string.

   \param begin beginning of selection
   \param end end of selection
   \param insert the string to insert
   \param append_mode can be one of REPLACE_MODE, INSERT_MODE or APPEND_MODE, affects the way the test update is performed
*/
static void replace_part(const editable_line_t &buffer,
                         const wchar_t *begin,
                         const wchar_t *end,
                         const wchar_t *insert,
                         int append_mode)
{
    const wchar_t *buff = buffer.text.c_str();
    size_t out_pos = buffer.position;

    wcstring out;

    out.append(buff, begin - buff);

    switch (append_mode)
    {
        case REPLACE_MODE:
        {

            out.append(insert);
            out_pos = wcslen(insert) + (begin-buff);
            break;

        }
        case APPEND_MODE:
        {
            out.append(begin, end-begin);
            out.append(insert);
            break;
        }
        case INSERT_MODE:
        {
            long cursor = buffer.position - (begin - buff);
            out.append(begin, cursor);
            out.append(insert);
            out.append(begin+cursor, end-begin-cursor);
            out_pos +=  wcslen(insert);
            break;
        }
    }
    out.append(end);
    
    editable_line_t tmp;
    tmp.text = out;
    tmp.position = out_pos;
    apply_new_commandline(tmp);
}

/**
   Output the specified selection.

   \param buffer the buffer containing the text to output
   \param cursor_position the position of the cursor
   \param begin start of selection
   \param end  end of selection
   \param cut_at_cursor whether printing should stop at the surrent cursor position
   \param tokenize whether the string should be tokenized, printing one string token on every line and skipping non-string tokens
*/
static void write_part(const wchar_t *buffer,
                       size_t cursor_pos,
                       io_streams_t &streams,
                       const wchar_t *begin,
                       const wchar_t *end,
                       int cut_at_cursor,
                       int tokenize)
{
    assert(buffer != NULL);
    size_t pos = cursor_pos - (begin - buffer);

    if (tokenize)
    {
        wchar_t *buff = wcsndup(begin, end-begin);
//    fwprintf( stderr, L"Subshell: %ls, end char %lc\n", buff, *end );
        wcstring out;
        tokenizer_t tok(buff, TOK_ACCEPT_UNFINISHED);
        for (; tok_has_next(&tok); tok_next(&tok))
        {
            if ((cut_at_cursor) &&
                    (tok_get_pos(&tok)+wcslen(tok_last(&tok)) >= pos))
                break;

            switch (tok_last_type(&tok))
            {
                case TOK_STRING:
                {
                    wcstring tmp = tok_last(&tok);
                    unescape_string_in_place(&tmp, UNESCAPE_INCOMPLETE);
                    out.append(tmp);
                    out.push_back(L'\n');
                    break;
                }

                default:
                {
                    break;
                }
            }
        }

        streams.stdout_stream.append(out);

        free(buff);
    }
    else
    {
        if (cut_at_cursor)
        {
            end = begin+pos;
        }

//    debug( 0, L"woot2 %ls -> %ls", buff, esc );
        wcstring tmp = wcstring(begin, end - begin);
        unescape_string_in_place(&tmp, UNESCAPE_INCOMPLETE);
        streams.stdout_stream.append(tmp);
        streams.stdout_stream.append(L"\n");
    }
}


/**
   The commandline builtin. It is used for specifying a new value for
   the commandline.
*/
static int builtin_commandline(parser_t &parser, io_streams_t &streams, wchar_t **argv)
{
    wgetopter_t w;
    int buffer_part=0;
    int cut_at_cursor=0;

    int argc = builtin_count_args(argv);
    int append_mode=0;

    int function_mode = 0;
    int selection_mode = 0;

    int tokenize = 0;

    int cursor_mode = 0;
    int line_mode = 0;
    int search_mode = 0;
    int paging_mode = 0;
    const wchar_t *begin = NULL, *end = NULL;
    
    const reader_snapshot_t snapshot = reader_get_last_snapshot();
    
    editable_line_t current_buffer;
    
    wcstring transient_commandline;
    if (parser.get_substituted_commandline(&transient_commandline))
    {
        current_buffer.text = transient_commandline;
        current_buffer.position = transient_commandline.size();
    }
    else
    {
        /* TODO: if we aren't interactive we ought to print an error here */
        current_buffer = snapshot.command_line;
    }

    w.woptind=0;

    while (1)
    {
        static const struct woption
                long_options[] =
        {
            { L"append", no_argument, 0, 'a' },
            { L"insert", no_argument, 0, 'i' },
            { L"replace", no_argument, 0, 'r' },
            { L"current-job", no_argument, 0, 'j' },
            { L"current-process", no_argument, 0, 'p' },
            { L"current-token", no_argument, 0, 't' },
            { L"current-buffer", no_argument, 0, 'b' },
            { L"cut-at-cursor", no_argument, 0, 'c' },
            { L"function", no_argument, 0, 'f' },
            { L"tokenize", no_argument, 0, 'o' },
            { L"help", no_argument, 0, 'h' },
            { L"input", required_argument, 0, 'I' },
            { L"cursor", no_argument, 0, 'C' },
            { L"line", no_argument, 0, 'L' },
            { L"search-mode", no_argument, 0, 'S' },
            { L"selection", no_argument, 0, 's' },
            { L"paging-mode", no_argument, 0, 'P' },
            { 0, 0, 0, 0 }
        };

        int opt_index = 0;

        int opt = w.wgetopt_long(argc,
                               argv,
                               L"abijpctwforhI:CLSsP",
                               long_options,
                               &opt_index);
        if (opt == -1)
            break;

        switch (opt)
        {
            case 0:
                if (long_options[opt_index].flag != 0)
                    break;
                streams.stderr_stream.append_format(BUILTIN_ERR_UNKNOWN,
                              argv[0],
                              long_options[opt_index].name);
                builtin_print_help(parser, streams, argv[0], streams.stderr_stream);

                return 1;

            case L'a':
                append_mode = APPEND_MODE;
                break;

            case L'b':
                buffer_part = STRING_MODE;
                break;


            case L'i':
                append_mode = INSERT_MODE;
                break;

            case L'r':
                append_mode = REPLACE_MODE;
                break;

            case 'c':
                cut_at_cursor=1;
                break;

            case 't':
                buffer_part = TOKEN_MODE;
                break;

            case 'j':
                buffer_part = JOB_MODE;
                break;

            case 'p':
                buffer_part = PROCESS_MODE;
                break;

            case 'f':
                function_mode=1;
                break;

            case 'o':
                tokenize=1;
                break;

            case 'I':
                current_buffer.text = w.woptarg;
                current_buffer.position = wcslen(w.woptarg);
                break;

            case 'C':
                cursor_mode = 1;
                break;

            case 'L':
                line_mode = 1;
                break;

            case 'S':
                search_mode = 1;
                break;

            case 's':
                selection_mode = 1;
                break;

            case 'P':
                paging_mode = 1;
                break;

            case 'h':
                builtin_print_help(parser, streams, argv[0], streams.stdout_stream);
                return 0;

            case L'?':
                builtin_unknown_option(parser, streams, argv[0], argv[w.woptind-1]);
                return 1;
        }
    }

    if (function_mode)
    {
        int i;

        /*
          Check for invalid switch combinations
        */
        if (buffer_part || cut_at_cursor || append_mode || tokenize || cursor_mode || line_mode || search_mode || paging_mode)
        {
            streams.stderr_stream.append_format(
                          BUILTIN_ERR_COMBO,
                          argv[0]);

            builtin_print_help(parser, streams, argv[0], streams.stderr_stream);
            return 1;
        }


        if (argc == w.woptind)
        {
            streams.stderr_stream.append_format(
                          BUILTIN_ERR_MISSING,
                          argv[0]);

            builtin_print_help(parser, streams, argv[0], streams.stderr_stream);
            return 1;
        }
        for (i=w.woptind; i<argc; i++)
        {
            wchar_t c = input_function_get_code(argv[i]);
            if (c != (wchar_t)(-1))
            {
                /*
                  input_unreadch inserts the specified keypress or
                  readline function at the back of the queue of unused
                  keypresses
                */
                input_queue_ch(c);
            }
            else
            {
                streams.stderr_stream.append_format(_(L"%ls: Unknown input function '%ls'\n"),
                              argv[0],
                              argv[i]);
                builtin_print_help(parser, streams, argv[0], streams.stderr_stream);
                return 1;
            }
        }

        return 0;
    }

    if (selection_mode)
    {
        // Note: this is a little sketchy because the selection and commandline may not be in sync here, since we take the lock twice
        // But the wcstring constructor below ensures that we won't get something like a buffer overrun
        const editable_line_t line = snapshot.command_line;
        if (snapshot.selection_is_active)
        {
            streams.stdout_stream.append(wcstring(line.text, snapshot.selection_start, snapshot.selection_length));
        }
        return 0;
    }

    /*
      Check for invalid switch combinations
    */
    if ((search_mode || line_mode || cursor_mode || paging_mode) && (argc-w.woptind > 1))
    {

        streams.stderr_stream.append_format(
                      argv[0],
                      L": Too many arguments\n",
                      NULL);
        builtin_print_help(parser, streams, argv[0], streams.stderr_stream);
        return 1;
    }

    if ((buffer_part || tokenize || cut_at_cursor) && (cursor_mode || line_mode || search_mode || paging_mode))
    {
        streams.stderr_stream.append_format(
                      BUILTIN_ERR_COMBO,
                      argv[0]);

        builtin_print_help(parser, streams, argv[0], streams.stderr_stream);
        return 1;
    }


    if ((tokenize || cut_at_cursor) && (argc-w.woptind))
    {
        streams.stderr_stream.append_format(
                      BUILTIN_ERR_COMBO2,
                      argv[0],
                      L"--cut-at-cursor and --tokenize can not be used when setting the commandline");


        builtin_print_help(parser, streams, argv[0], streams.stderr_stream);
        return 1;
    }

    if (append_mode && !(argc-w.woptind))
    {
        streams.stderr_stream.append_format(
                      BUILTIN_ERR_COMBO2,
                      argv[0],
                      L"insertion mode switches can not be used when not in insertion mode");

        builtin_print_help(parser, streams, argv[0], streams.stderr_stream);
        return 1;
    }

    /*
      Set default modes
    */
    if (!append_mode)
    {
        append_mode = REPLACE_MODE;
    }

    if (!buffer_part)
    {
        buffer_part = STRING_MODE;
    }

    if (cursor_mode)
    {
        if (argc-w.woptind)
        {
            wchar_t *endptr;
            long new_pos;
            errno = 0;

            new_pos = wcstol(argv[w.woptind], &endptr, 10);
            if (*endptr || errno)
            {
                streams.stderr_stream.append_format(BUILTIN_ERR_NOT_NUMBER,
                              argv[0],
                              argv[w.woptind]);
                builtin_print_help(parser, streams, argv[0], streams.stderr_stream);
            }

            current_buffer = snapshot.command_line;
            current_buffer.position = (size_t)maxi(0L, mini(new_pos, (long)current_buffer.text.size()));
            apply_new_commandline(current_buffer);
            return 0;
        }
        else
        {
            streams.stdout_stream.append_format(L"%lu\n", current_buffer.position);
            return 0;
        }

    }

    if (line_mode)
    {
        const editable_line_t line = snapshot.command_line;
        size_t pos = line.position;
        const wchar_t *buff = line.text.c_str();
        streams.stdout_stream.append_format(L"%lu\n", (unsigned long)parse_util_lineno(buff, pos));
        return 0;

    }

    if (search_mode)
    {
        return ! snapshot.search_mode;
    }

    if (paging_mode)
    {
        return ! snapshot.has_pager_contents;
    }


    switch (buffer_part)
    {
        case STRING_MODE:
        {
            begin = current_buffer.text.c_str();
            end = begin+wcslen(begin);
            break;
        }

        case PROCESS_MODE:
        {
            parse_util_process_extent(current_buffer.text.c_str(),
                                      current_buffer.position,
                                      &begin,
                                      &end);
            break;
        }

        case JOB_MODE:
        {
            parse_util_job_extent(current_buffer.text.c_str(),
                                  current_buffer.position,
                                  &begin,
                                  &end);
            break;
        }

        case TOKEN_MODE:
        {
            parse_util_token_extent(current_buffer.text.c_str(),
                                    current_buffer.position,
                                    &begin,
                                    &end,
                                    0, 0);
            break;
        }

    }

    switch (argc-w.woptind)
    {
        case 0:
        {
            write_part(current_buffer.text.c_str(), current_buffer.position, streams, begin, end, cut_at_cursor, tokenize);
            break;
        }

        case 1:
        {
            replace_part(current_buffer, begin, end, argv[w.woptind], append_mode);
            break;
        }

        default:
        {
            wcstring sb = argv[w.woptind];
            int i;

            for (i=w.woptind+1; i<argc; i++)
            {
                sb.push_back(L'\n');
                sb.append(argv[i]);
            }

            replace_part(current_buffer, begin, end, sb.c_str(), append_mode);

            break;
        }
    }

    return 0;
}
