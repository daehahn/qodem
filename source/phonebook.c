/*
 * phonebook.c
 *
 * This module is licensed under the GNU General Public License
 * Version 2.  Please see the file "COPYING" in this directory for
 * more information about the GNU General Public License Version 2.
 *
 *     Copyright (C) 2015  Kevin Lamonte
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301 USA
 */

#include "common.h"
#include <assert.h>
#include <ctype.h>
#ifdef __BORLANDC__
#include <stdio.h>
#else
#include <wctype.h>
#include <unistd.h>
#endif
#include <libgen.h>     /* basename */
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "qodem.h"
#include "screen.h"
#include "forms.h"
#include "keyboard.h"
#include "console.h"
#include "states.h"
#include "options.h"
#include "music.h"
#include "phonebook.h"
#include "field.h"
#include "script.h"
#include "dialer.h"
#include "help.h"
#include "netclient.h"

/* Modem dialer ----------------------------------------------------------- */

/* #define DEBUG_DIALER 1 */
#undef DEBUG_DIALER
#ifdef DEBUG_DIALER
static FILE * DEBUG_FILE_HANDLE = NULL;
#endif

typedef enum {
        DIAL_MODEM_INIT,
        DIAL_MODEM_SENT_AT,
        DIAL_MODEM_SENT_DIAL_STRING,
        DIAL_MODEM_CONNECTED
} DIAL_MODEM_STATE;

#ifndef Q_NO_SERIAL
/* Current modem */
static DIAL_MODEM_STATE modem_state;
#endif /* Q_NO_SERIAL */

/* ------------------------------------------------------------------------ */

/* Max size of a displayed emulation string name */
#define EMULATION_STRING_SIZE   32

/* Max size of a displayed codepage string name */
#define CODEPAGE_STRING_SIZE    32

/* Max size of a displayed connection method string name */
#define METHOD_STRING_SIZE      32

/* Currently-connected entry */
struct q_phone_struct * q_current_dial_entry = NULL;

/* The phonebook. */
struct q_phonebook_struct q_phonebook = {
        "fonebook.txt",                 /* filename */
        0,                              /* tagged */
        0,                              /* view_mode */
        NULL,                           /* entries */
        0,                              /* entry_count */
        NULL                            /* selected_entry */
};

/* The currently selected entry in the phonebook */
static int phonebook_entry_i = 0;

/* The currently visible "page" in the phonebook */
static int phonebook_page = 0;

/* Sort field choices */
typedef enum {
        SORT_METHOD_NAME_ASC,           /* Sort by name ascending */
        SORT_METHOD_ADDRESS_ASC,        /* Sort by address ascending */
        SORT_METHOD_TOTAL_CALLS_DESC,   /* Sort by total calls descending */
        SORT_METHOD_METHOD_ASC,         /* Sort by method ascending */
        SORT_METHOD_LAST_CALL_DESC,     /* Sort by method last call descending */
        SORT_METHOD_REVERSE,            /* Reverse existing order */

        SORT_METHOD_MAX
} SORT_METHOD;

/* If the phonebook is printed to this "file", run it through cat instead */
#define LPR_FILE_NAME "|lpr"

/*
 * When true, phonebook_refresh() pops up a notification that find/find again
 * found the text in a note.
 */
static Q_BOOL found_note_flag = Q_FALSE;

/* Close the currently dialed entry */
static void close_dial_entry() {
        if (q_current_dial_entry != NULL) {
#ifndef Q_NO_SERIAL
                if (q_current_dial_entry->method == Q_DIAL_METHOD_MODEM) {
                        close_serial_port();
                } else {
#endif /* Q_NO_SERIAL */
                        net_close();
#ifdef __BORLANDC__
                        closesocket(q_child_tty_fd);
#else
                        close(q_child_tty_fd);
#endif
                        q_child_tty_fd = -1;
#ifndef Q_NO_SERIAL
                }
#endif /* Q_NO_SERIAL */
        }
} /* ---------------------------------------------------------------------- */

/*
 * Reset the phonebook selection display (for when KEY_RESIZE comes in)
 */
void phonebook_reset() {
        phonebook_entry_i = 0;
        phonebook_page = 0;
        q_phonebook.selected_entry = q_phonebook.entries;
} /* ---------------------------------------------------------------------- */

/*
 * Fixup phonebook_entry_i and phonebook_page to match the current selected entry
 */
void phonebook_normalize() {
        int visible_entries_n = HEIGHT - 1 - 14;
        struct q_phone_struct * p;

        phonebook_entry_i = 0;
        phonebook_page = 0;

        if ((q_phonebook.selected_entry == NULL) || (q_phonebook.entries == NULL)) {
                return;
        }

        p = q_phonebook.entries;
        while (p != NULL) {
                if (p == q_phonebook.selected_entry) {
                        return;
                }
                phonebook_entry_i++;
                if (phonebook_entry_i % visible_entries_n == 0) {
                        phonebook_page++;
                }
                p = p->next;
        }
} /* ---------------------------------------------------------------------- */

/*
 * Print the phonebook in 80-column mode
 */
static void print_phonebook_80(const char * dest) {
        char notify_message[DIALOG_MESSAGE_SIZE];
        char command[COMMAND_LINE_SIZE];
        char line[80 + 1];
        FILE * file;
        char * filename;
        char * full_filename;
        Q_BOOL lpr = Q_FALSE;
        struct q_phone_struct * entry;
        int left_stop;
        int i;
        int entry_i;
        int page = 1;
        Q_BOOL page_header = Q_TRUE;
        int lines_per_page = 60;
        int lines = 0;

        if (strstr(dest, LPR_FILE_NAME) == dest) {
                /* This one is going to lpr */
                lpr = Q_TRUE;
                filename = "savefon.txt";
        } else {
                lpr = Q_FALSE;
                filename = (char *)dest;
        }

        /* Print everything to file first */
        file = open_workingdir_file(filename, &full_filename);
        if (file == NULL) {
                snprintf(notify_message, sizeof(notify_message), _("Error opening file \"%s\" for writing: %s"), full_filename, strerror(errno));
                notify_form(notify_message, 0);
                if (full_filename != NULL) {
                        Xfree(full_filename, __FILE__, __LINE__);
                }
                return;
        }

        /* Start at the top */
        entry = q_phonebook.entries;

        sprintf(line, "%s %s %s", _("Qodem Version"), Q_VERSION , _("Phone Book"));
        left_stop = (80 - strlen(line)) / 2;
        for (i = 0; i < left_stop; i++) {
                fprintf(file, " ");
        }
        fprintf(file, "%s\n", line);
        for (i = 0; i < left_stop; i++) {
                fprintf(file, " ");
        }
        for (i = 0; i < strlen(line); i++) {
                fprintf(file, "=");
        }
        fprintf(file, "\n\n");
        lines += 3;

        /* Loop through the entries */
        entry_i = 1;
        while (entry != NULL) {
                if (page_header == Q_TRUE) {
                        fprintf(file, _("Page %2d   File : %s"), page, full_filename);
                        fprintf(file, "\n");
                        fprintf(file, "\n");
                        fprintf(file, _("              Name                        Address/Number    Method  Com Settings"));
                        fprintf(file, "\n");
                        for (i = 0; i < 80; i++) {
                                fprintf(file, "-");
                        }
                        fprintf(file, "\n");
                        lines += 4;

                        page++;
                        page_header = Q_FALSE;
                }

#ifndef Q_NO_SERIAL
                if (entry->use_modem_cfg == Q_TRUE) {
                        snprintf(line, sizeof(line) - 1, _(" Modem Cfg"));
                } else {
                        snprintf(line, sizeof(line) - 1,
                                "%6.6s %s-%s-%s",
                                baud_string(entry->baud),
                                data_bits_string(entry->data_bits),
                                parity_string(entry->parity, Q_TRUE),
                                stop_bits_string(entry->stop_bits));
                }
#endif /* Q_NO_SERIAL */

                fprintf(file, _("%3d %-28.28ls %23.23s %9s %9s"),
                        entry_i, entry->name, entry->address, method_string(entry->method), line);
                fprintf(file, "\n");
                lines++;

                if (lines == lines_per_page) {
                        /* Send form feed */
                        fprintf(file, "\014\n");

                        /* Reset for next page */
                        lines = 0;
                        page_header = Q_TRUE;
                }

                /* Next entry */
                entry = entry->next;
                entry_i++;
        }

        /* Send form feed */
        fprintf(file, "\014");

        fclose(file);
        if (full_filename != filename) {
                Xfree(full_filename, __FILE__, __LINE__);
        }

        if (lpr == Q_TRUE) {
                /* Send the file to lpr */
                memset(command, 0, sizeof(command));
                snprintf(command, sizeof(command) - 1, "cat %s | lpr", full_filename);
                system(command);
        }
} /* ---------------------------------------------------------------------- */

/*
 * Print the phonebook in 132-column mode
 */
static void print_phonebook_132(const char * dest) {
        char notify_message[DIALOG_MESSAGE_SIZE];
        char command[COMMAND_LINE_SIZE];
        char line[132 + 1];
        FILE * file;
        char * filename;
        char * full_filename;
        Q_BOOL lpr = Q_FALSE;
        struct q_phone_struct * entry;
        int left_stop;
        int i;
        int entry_i;
        int page = 1;
        Q_BOOL page_header = Q_TRUE;
        int lines_per_page = 60;
        int lines = 0;

        if (strstr(dest, LPR_FILE_NAME) == dest) {
                /* This one is going to lpr */
                lpr = Q_TRUE;
                filename = "savefon.txt";
        } else {
                lpr = Q_FALSE;
                filename = (char *)dest;
        }

        /* Print everything to file first */
        file = open_workingdir_file(filename, &full_filename);
        if (file == NULL) {
                snprintf(notify_message, sizeof(notify_message), _("Error opening file \"%s\" for writing: %s"), full_filename, strerror(errno));
                notify_form(notify_message, 0);
                if (full_filename != NULL) {
                        Xfree(full_filename, __FILE__, __LINE__);
                }
                return;
        }

        /* Start at the top */
        entry = q_phonebook.entries;

        sprintf(line, "%s %s %s", _("Qodem Version"), Q_VERSION , _("Phone Book"));
        left_stop = (132 - strlen(line)) / 2;
        for (i = 0; i < left_stop; i++) {
                fprintf(file, " ");
        }
        fprintf(file, "%s\n", line);
        for (i = 0; i < left_stop; i++) {
                fprintf(file, " ");
        }
        for (i = 0; i < strlen(line); i++) {
                fprintf(file, "=");
        }
        fprintf(file, "\n\n");
        lines += 3;

        /* Loop through the entries */
        entry_i = 1;
        while (entry != NULL) {
                if (page_header == Q_TRUE) {
                        fprintf(file, _("Page %2d   File : %s"), page, full_filename);
                        fprintf(file, "\n");
                        fprintf(file, "\n");
                        fprintf(file, _(
"              Name                        Address/Number  Port   Method  Com Settings  Emulation         Username           Password"));
                        fprintf(file, "\n");
                        for (i = 0; i < 132; i++) {
                                fprintf(file, "-");
                        }
                        fprintf(file, "\n");
                        lines += 4;

                        page++;
                        page_header = Q_FALSE;
                }

#ifndef Q_NO_SERIAL
                if (entry->use_modem_cfg == Q_TRUE) {
                        snprintf(line, sizeof(line) - 1, _(" Modem Cfg"));
                } else {
                        snprintf(line, sizeof(line) - 1,
                                "%6.6s %s-%s-%s",
                                baud_string(entry->baud),
                                data_bits_string(entry->data_bits),
                                parity_string(entry->parity, Q_TRUE),
                                stop_bits_string(entry->stop_bits));
                }
#endif /* Q_NO_SERIAL */

                fprintf(file, _("%3d %-28.28ls %23.23s %5s %7s %13.13s %8.8s %19.19ls %18.18ls"),
                        entry_i, entry->name, entry->address, entry->port,
                        method_string(entry->method), line,
                        emulation_string(entry->emulation),
                        entry->username, entry->password
                );
                fprintf(file, "\n");
                lines++;

                if (lines == lines_per_page) {
                        /* Send form feed */
                        fprintf(file, "\014\n");

                        /* Reset for next page */
                        lines = 0;
                        page_header = Q_TRUE;
                }

                /* Next entry */
                entry = entry->next;
                entry_i++;
        }

        /* Send form feed */
        fprintf(file, "\014");

        fclose(file);
        if (full_filename != filename) {
                Xfree(full_filename, __FILE__, __LINE__);
        }

        if (lpr == Q_TRUE) {
                /* Send the file to lpr */
                memset(command, 0, sizeof(command));
                snprintf(command, sizeof(command) - 1, "cat %s | lpr", full_filename);
                system(command);
        }
} /* ---------------------------------------------------------------------- */

/*
 * default_port - return the default port # AS A MALLOC'D STRING for method
 */
static char * default_port(const Q_DIAL_METHOD method) {

        switch (method) {
        case Q_DIAL_METHOD_SOCKET:
                /* Default on the telnet port */
                return Xstrdup("23", __FILE__, __LINE__);
        case Q_DIAL_METHOD_TELNET:
                return Xstrdup("23", __FILE__, __LINE__);
        case Q_DIAL_METHOD_SSH:
                return Xstrdup("22", __FILE__, __LINE__);
        case Q_DIAL_METHOD_RLOGIN:
                return Xstrdup("", __FILE__, __LINE__);
        case Q_DIAL_METHOD_SHELL:
                return Xstrdup("", __FILE__, __LINE__);
        case Q_DIAL_METHOD_COMMANDLINE:
                return Xstrdup("", __FILE__, __LINE__);
#ifndef Q_NO_SERIAL
        case Q_DIAL_METHOD_MODEM:
                return Xstrdup("", __FILE__, __LINE__);
#endif /* Q_NO_SERIAL */
        }
        return NULL;
} /* ---------------------------------------------------------------------- */

/*
 * doorway_string - return the string representing the doorway option
 */
static char * doorway_string(const Q_DOORWAY doorway) {

        switch (doorway) {

        case Q_DOORWAY_CONFIG:
                return _("Use Global Option");

        case Q_DOORWAY_ALWAYS_DOORWAY:
                return _("Always DOORWAY");

        case Q_DOORWAY_ALWAYS_MIXED:
                return _("Always MIXED");

        case Q_DOORWAY_NEVER:
                return _("Never");
        }
        return NULL;
} /* ---------------------------------------------------------------------- */

/*
 * doorway_from_string - return a doorway option given the string representation
 */
static Q_DOORWAY doorway_from_string(const char * string) {

        if (strncmp(string, _("Use Global Option"), strlen(_("Use Global Option"))) == 0) {
                return Q_DOORWAY_CONFIG;
        } else if (strncmp(string, _("Always DOORWAY"), strlen(_("Always DOORWAY"))) == 0) {
                return Q_DOORWAY_ALWAYS_DOORWAY;
        } else if (strncmp(string, _("Always MIXED"), strlen(_("Always MIXED"))) == 0) {
                return Q_DOORWAY_ALWAYS_MIXED;
        } else if (strncmp(string, _("Never"), strlen(_("Never"))) == 0) {
                return Q_DOORWAY_NEVER;
        }

        return -1;
} /* ---------------------------------------------------------------------- */


/*
 * method_string - return the string representing the connection method
 */
char * method_string(const Q_DIAL_METHOD method) {

        switch (method) {
/*
        case Q_DIAL_METHOD_SHELL:
                return _("LOCAL");
        case Q_DIAL_METHOD_SSH:
                return _("SSH");
        case Q_DIAL_METHOD_RLOGIN:
                return _("RLOGIN");
        case Q_DIAL_METHOD_TELNET:
                return _("TELNET");
        case Q_DIAL_METHOD_TN3270:
                return _("TN3270");
*/
        case Q_DIAL_METHOD_SHELL:
                return "LOCAL";
#ifndef Q_NO_SERIAL
        case Q_DIAL_METHOD_MODEM:
                return "MODEM";
#endif /* Q_NO_SERIAL */
        case Q_DIAL_METHOD_SSH:
                return "SSH";
        case Q_DIAL_METHOD_RLOGIN:
                return "RLOGIN";
        case Q_DIAL_METHOD_TELNET:
                return "TELNET";
        case Q_DIAL_METHOD_SOCKET:
                return "SOCKET";
        /*
        case Q_DIAL_METHOD_TN3270:
                return "TN3270";
         */
        case Q_DIAL_METHOD_COMMANDLINE:
                return "CMDLINE";
        }

        return NULL;
} /* ---------------------------------------------------------------------- */

/*
 * method_from_string - return a connection method given the string representation
 */
static Q_DIAL_METHOD method_from_string(const char * string) {

/*
        if (strncmp(string, _("LOCAL"), strlen(_("LOCAL"))) == 0) {
                return Q_DIAL_METHOD_SHELL;
        } else if (strncmp(string, _("SSH"), strlen(_("SSH"))) == 0) {
                return Q_DIAL_METHOD_SSH;
        } else if (strncmp(string, _("RLOGIN"), strlen(_("RLOGIN"))) == 0) {
                return Q_DIAL_METHOD_RLOGIN;
        } else if (strncmp(string, _("TELNET"), strlen(_("TELNET"))) == 0) {
                return Q_DIAL_METHOD_TELNET;
        } else if (strncmp(string, _("TN3270"), strlen(_("TN3270"))) == 0) {
                return Q_DIAL_METHOD_TN3270;
        }
*/

        if (strncmp(string, "LOCAL", strlen("LOCAL")) == 0) {
                return Q_DIAL_METHOD_SHELL;
        } else if (strncmp(string, "SSH", strlen("SSH")) == 0) {
                return Q_DIAL_METHOD_SSH;
#ifndef Q_NO_SERIAL
        } else if (strncmp(string, "MODEM", strlen("MODEM")) == 0) {
                return Q_DIAL_METHOD_MODEM;
#endif /* Q_NO_SERIAL */
        } else if (strncmp(string, "RLOGIN", strlen("RLOGIN")) == 0) {
                return Q_DIAL_METHOD_RLOGIN;
        } else if (strncmp(string, "TELNET", strlen("TELNET")) == 0) {
                return Q_DIAL_METHOD_TELNET;
        } else if (strncmp(string, "SOCKET", strlen("SOCKET")) == 0) {
                return Q_DIAL_METHOD_SOCKET;
        /*
        } else if (strncmp(string, "TN3270", strlen("TN3270")) == 0) {
                return Q_DIAL_METHOD_TN3270;
         */
        } else if (strncmp(string, "CMDLINE", strlen("CMDLINE")) == 0) {
                return Q_DIAL_METHOD_COMMANDLINE;
        }

        return -1;
} /* ---------------------------------------------------------------------- */

#define TOGGLE_SESSION_LOG      0x01
#define TOGGLE_XONXOFF          0x02
#define TOGGLE_HARD_BACKSPACE   0x04
#define TOGGLE_LINEWRAP         0x08
#define TOGGLE_DISPLAY_NULL     0x10
#define TOGGLE_STATUS_LINE_INFO 0x20
#define TOGGLE_STRIP_8TH        0x40
#define TOGGLE_BEEPS            0x80
#define TOGGLE_HALF_DUPLEX      0x100
#define TOGGLE_SCROLLBACK       0x200
#define TOGGLE_STATUS_LINE      0x400
#define TOGGLE_CRLF             0x800
#define TOGGLE_ANSI_MUSIC       0x1000

static char toggles_string_buffer[32];

/*
 * Set global toggles based on toggles bitmask.  Each toggle is set
 * to the opposite of the defaults (set in qodem.c).
 */
void set_dial_out_toggles(int toggles) {
        if (toggles & TOGGLE_SESSION_LOG) {
                if (q_status.logging == Q_FALSE) {
                        start_logging(get_option(Q_OPTION_LOG_FILE));
                }
        }

#ifndef Q_NO_SERIAL
        if (toggles & TOGGLE_XONXOFF) {
                q_serial_port.xonxoff = Q_TRUE;
                q_cursor_on();
        } else {
                q_serial_port.xonxoff = Q_FALSE;
                q_cursor_on();
        }
        /* We edited something, reconfigure the port if it's open */
        if (q_status.serial_open == Q_TRUE) {
                if (configure_serial_port() == Q_FALSE) {
                        /* notify_form() just turned off the cursor */
                        q_cursor_on();
                }
        }
#endif /* Q_NO_SERIAL */

        if (toggles & TOGGLE_HARD_BACKSPACE) {
                q_status.hard_backspace = Q_TRUE;
        } else {
                q_status.hard_backspace = Q_FALSE;
        }

        if (toggles & TOGGLE_LINEWRAP) {
                q_status.line_wrap = Q_FALSE;
        } else {
                q_status.line_wrap = Q_TRUE;
        }

        if (toggles & TOGGLE_DISPLAY_NULL) {
                q_status.display_null = Q_TRUE;
        } else {
                q_status.display_null = Q_FALSE;
        }

        if (toggles & TOGGLE_STATUS_LINE_INFO) {
                q_status.status_line_info = Q_TRUE;
        } else {
                q_status.status_line_info = Q_FALSE;
        }

        if (toggles & TOGGLE_STRIP_8TH) {
                q_status.strip_8th_bit = Q_TRUE;
        } else {
                q_status.strip_8th_bit = Q_FALSE;
        }

        if (toggles & TOGGLE_BEEPS) {
                q_status.beeps = Q_FALSE;
        } else {
                q_status.beeps = Q_TRUE;
        }

        if (toggles & TOGGLE_HALF_DUPLEX) {
                q_status.full_duplex = Q_FALSE;
        } else {
                q_status.full_duplex = Q_TRUE;
        }

        if (toggles & TOGGLE_SCROLLBACK) {
                q_status.scrollback_enabled = Q_FALSE;
        } else {
                q_status.scrollback_enabled = Q_TRUE;
        }

        if (toggles & TOGGLE_STATUS_LINE) {
                set_status_line(Q_FALSE);
        } else {
                set_status_line(Q_TRUE);
        }

        if (toggles & TOGGLE_CRLF) {
                q_status.line_feed_on_cr = Q_TRUE;
        } else {
                q_status.line_feed_on_cr = Q_FALSE;
        }

        if (toggles & TOGGLE_ANSI_MUSIC) {
                q_status.ansi_music = Q_FALSE;
        } else {
                q_status.ansi_music = Q_TRUE;
        }

} /* ---------------------------------------------------------------------- */

/* Prompt for new toggles */
static char * toggles_to_string(int toggles) {
        memset(toggles_string_buffer, 0, sizeof(toggles_string_buffer));

        if (toggles & TOGGLE_SESSION_LOG) {
                toggles_string_buffer[strlen(toggles_string_buffer)] = '0';
        }
        if (toggles & TOGGLE_XONXOFF) {
                toggles_string_buffer[strlen(toggles_string_buffer)] = '1';
        }
        if (toggles & TOGGLE_HARD_BACKSPACE) {
                toggles_string_buffer[strlen(toggles_string_buffer)] = '2';
        }
        if (toggles & TOGGLE_LINEWRAP) {
                toggles_string_buffer[strlen(toggles_string_buffer)] = '3';
        }
        if (toggles & TOGGLE_DISPLAY_NULL) {
                toggles_string_buffer[strlen(toggles_string_buffer)] = '4';
        }
        if (toggles & TOGGLE_STATUS_LINE_INFO) {
                toggles_string_buffer[strlen(toggles_string_buffer)] = '7';
        }
        if (toggles & TOGGLE_STRIP_8TH) {
                toggles_string_buffer[strlen(toggles_string_buffer)] = '8';
        }
        if (toggles & TOGGLE_BEEPS) {
                toggles_string_buffer[strlen(toggles_string_buffer)] = 'B';
        }
        if (toggles & TOGGLE_HALF_DUPLEX) {
                toggles_string_buffer[strlen(toggles_string_buffer)] = 'E';
        }
        if (toggles & TOGGLE_SCROLLBACK) {
                toggles_string_buffer[strlen(toggles_string_buffer)] = 'U';
        }
        if (toggles & TOGGLE_STATUS_LINE) {
                toggles_string_buffer[strlen(toggles_string_buffer)] = '-';
        }
        if (toggles & TOGGLE_CRLF) {
                toggles_string_buffer[strlen(toggles_string_buffer)] = '+';
        }
        if (toggles & TOGGLE_ANSI_MUSIC) {
                toggles_string_buffer[strlen(toggles_string_buffer)] = ',';
        }

        return toggles_string_buffer;
} /* ---------------------------------------------------------------------- */

/*
 * load_phonebook
 */
void load_phonebook(const Q_BOOL backup_version) {
        FILE * file;
        char * begin;
        char * end;
        char * filename;
        int i;
        char line[PHONEBOOK_LINE_SIZE];
        char buffer[PHONEBOOK_LINE_SIZE];
        wchar_t value_wchar[PHONEBOOK_LINE_SIZE];
        struct q_phone_struct * old_entry;
        struct q_phone_struct * new_entry;

        enum SCAN_STATES {
                SCAN_STATE_NONE,        /* Between entries */
                SCAN_STATE_ENTRY,       /* Scanning for single-line fields */
                SCAN_STATE_NOTES        /* Reading the notes */
        };

        int scan_state;
        int notes_length = 0;

        if (backup_version == Q_FALSE) {
                filename = q_phonebook.filename;
        } else {
                filename = (char *)Xmalloc(sizeof(char) * (strlen(q_phonebook.filename) + 5), __FILE__, __LINE__);
                snprintf(filename, strlen(q_phonebook.filename) + 5, "%s.bak", q_phonebook.filename);
        }

        file = fopen(filename, "r");
        if (file == NULL) {
                snprintf(line, sizeof(line), _("Error opening file \"%s\" for reading: %s"), filename, strerror(errno));
                notify_form(line, 0);
                if (backup_version == Q_TRUE) {
                        Xfree(filename, __FILE__, __LINE__);
                }
                return;
        }

        /* Free old phonebook */
        old_entry = q_phonebook.entries;
        while (old_entry != NULL) {
                Xfree(old_entry->name, __FILE__, __LINE__);
                Xfree(old_entry->address, __FILE__, __LINE__);
                Xfree(old_entry->port, __FILE__, __LINE__);
                Xfree(old_entry->username, __FILE__, __LINE__);
                Xfree(old_entry->password, __FILE__, __LINE__);
                if (old_entry->notes != NULL) {
                        for (i=0; old_entry->notes[i] != NULL; i++) {
                                Xfree(old_entry->notes[i], __FILE__, __LINE__);
                        }
                        Xfree(old_entry->notes, __FILE__, __LINE__);
                }
                Xfree(old_entry->script_filename, __FILE__, __LINE__);
                Xfree(old_entry->capture_filename, __FILE__, __LINE__);
                Xfree(old_entry->keybindings_filename, __FILE__, __LINE__);
                new_entry = old_entry;
                old_entry = old_entry->next;
                Xfree(new_entry, __FILE__, __LINE__);
        }

        /* Reset for new phonebook */
        q_phonebook.tagged              = 0;
        q_phonebook.view_mode           = 0;
        q_phonebook.entries             = 0;
        q_phonebook.entry_count         = 0;
        q_phonebook.selected_entry      = NULL;

        old_entry = NULL;
        new_entry = NULL;
        scan_state = SCAN_STATE_NONE;
        while (!feof(file)) {

                if (fgets(line, sizeof(line), file) == NULL) {
                        /* This should cause the outer while's feof() check to fail */
                        continue;
                }
                begin = line;

                if (scan_state == SCAN_STATE_NONE) {
                        while ((strlen(line) > 0) && isspace(line[strlen(line)-1])) {
                                /* Trim trailing whitespace */
                                line[strlen(line)-1] = '\0';
                        }
                        while (isspace(*begin)) {
                                /* Trim leading whitespace */
                                begin++;
                        }

                        if ((*begin == '#') || (strlen(begin) == 0)) {
                                /* Ignore blank lines and commented lines between entries */
                                continue;
                        }

                        if (strncmp(begin, "[entry]", sizeof("[entry]")) == 0) {
                                /* Beginning of an entry found */
                                scan_state = SCAN_STATE_ENTRY;
                                new_entry = (struct q_phone_struct *)Xmalloc(sizeof(struct q_phone_struct), __FILE__, __LINE__);
                                memset(new_entry, 0, sizeof(struct q_phone_struct));
                                if (q_phonebook.entry_count == 0) {
                                        q_phonebook.entries = new_entry;
                                        new_entry->prev = NULL;
                                } else {
                                        old_entry->next = new_entry;
                                        new_entry->prev = old_entry;

                                        /* Fixup any missing fields */
                                        if (old_entry->script_filename == NULL) {
                                                old_entry->script_filename = Xstrdup("", __FILE__, __LINE__);
                                        }
                                        if (old_entry->capture_filename == NULL) {
                                                old_entry->capture_filename = Xstrdup("", __FILE__, __LINE__);
                                        }
                                        if (old_entry->keybindings_filename == NULL) {
                                                old_entry->keybindings_filename = Xstrdup("", __FILE__, __LINE__);
                                        }
                                        if (old_entry->username == NULL) {
                                                old_entry->username = Xwcsdup(L"", __FILE__, __LINE__);
                                        }
                                        if (old_entry->password == NULL) {
                                                old_entry->password = Xwcsdup(L"", __FILE__, __LINE__);
                                        }
                                        if (old_entry->name == NULL) {
                                                old_entry->name = Xwcsdup(L"", __FILE__, __LINE__);
                                        }
                                        if (old_entry->address == NULL) {
                                                old_entry->address = Xstrdup("", __FILE__, __LINE__);
                                        }
                                        if (old_entry->port == NULL) {
                                                old_entry->port = Xstrdup("", __FILE__, __LINE__);
                                        }
                                }

                                q_phonebook.entry_count++;
                                new_entry->port                 = NULL;
                                new_entry->notes                = NULL;
                                new_entry->tagged               = Q_FALSE;
                                new_entry->doorway              = Q_DOORWAY_CONFIG;
                                new_entry->emulation            = Q_EMUL_VT102;
                                new_entry->codepage             = default_codepage(new_entry->emulation);

#ifndef Q_NO_SERIAL
                                new_entry->use_modem_cfg        = Q_TRUE;
                                new_entry->baud                 = Q_BAUD_115200;
                                new_entry->data_bits            = DATA_BITS_8;
                                new_entry->parity               = Q_PARITY_NONE;
                                new_entry->stop_bits            = STOP_BITS_1;
                                new_entry->xonxoff              = Q_FALSE;
                                new_entry->rtscts               = Q_TRUE;
                                new_entry->lock_dte_baud        = Q_TRUE;
#endif /* Q_NO_SERIAL */

                                new_entry->name                 = NULL;
                                new_entry->address              = NULL;
                                new_entry->port                 = NULL;
                                new_entry->username             = NULL;
                                new_entry->password             = NULL;
                                new_entry->script_filename      = NULL;
                                new_entry->capture_filename     = NULL;
                                new_entry->keybindings_filename = NULL;
                                new_entry->use_default_toggles  = Q_TRUE;
                                new_entry->toggles              = 0;
                                new_entry->quicklearn           = Q_FALSE;

                                new_entry->next                 = NULL;

                                /* Save entry */
                                old_entry = new_entry;
                                continue;
                        }
                }

                if (scan_state == SCAN_STATE_ENTRY) {
                        while ((strlen(line) > 0) && isspace(line[strlen(line)-1])) {
                                /* Trim trailing whitespace */
                                line[strlen(line)-1] = '\0';
                        }
                        while (isspace(*begin)) {
                                /* Trim leading whitespace */
                                begin++;
                        }

                        if ((*begin == '#') || (strlen(begin) == 0)) {
                                /* Ignore blank lines and commented lines between entries */
                                continue;
                        }

                        end = strchr(begin, '=');
                        if (end == NULL) {
                                /* Ignore this line. */
                                continue;
                        }
                        memset(buffer, 0, sizeof(buffer));
                        strncpy(buffer, begin, end - begin);
                        begin = end + 1;

                        mbstowcs(value_wchar, begin, strlen(begin) + 1);

                        if (strncmp(buffer, "name", strlen("name")) == 0) {
                                /* NAME */
                                new_entry->name = Xwcsdup(value_wchar, __FILE__, __LINE__);
                        } else if (strncmp(buffer, "address", strlen("address")) == 0) {
                                /* ADDRESS */
                                new_entry->address = Xstrdup(begin, __FILE__, __LINE__);
                        } else if (strncmp(buffer, "port", strlen("port")) == 0) {
                                /* PORT */
                                new_entry->port = Xstrdup(begin, __FILE__, __LINE__);
                        } else if (strncmp(buffer, "username", strlen("password")) == 0) {
                                /* USERNAME */
                                new_entry->username = Xwcsdup(value_wchar, __FILE__, __LINE__);
                        } else if (strncmp(buffer, "password", strlen("password")) == 0) {
                                /* PASSWORD */
                                new_entry->password = Xwcsdup(value_wchar, __FILE__, __LINE__);
                        } else if (strncmp(buffer, "tagged", strlen("tagged")) == 0) {
                                /* TAGGED */
                                if (strncasecmp(begin, "true", strlen("true")) == 0) {
                                        new_entry->tagged = Q_TRUE;
                                        q_phonebook.tagged++;
                                }
                        } else if (strncmp(buffer, "doorway", strlen("doorway")) == 0) {
                                /* TAGGED */
                                if (strncasecmp(begin, "doorway", strlen("doorway")) == 0) {
                                        new_entry->doorway = Q_DOORWAY_ALWAYS_DOORWAY;
                                } else if (strncasecmp(begin, "always", strlen("always")) == 0) {
                                        /* Allow "always" to mean always doorway too, for upgrading */
                                        new_entry->doorway = Q_DOORWAY_ALWAYS_DOORWAY;
                                } else if (strncasecmp(begin, "mixed", strlen("mixed")) == 0) {
                                        new_entry->doorway = Q_DOORWAY_ALWAYS_MIXED;
                                } else if (strncasecmp(begin, "never", strlen("never")) == 0) {
                                        new_entry->doorway = Q_DOORWAY_NEVER;
                                } else {
                                        new_entry->doorway = Q_DOORWAY_CONFIG;
                                }
                        } else if (strncmp(buffer, "method", strlen("method")) == 0) {
                                /* METHOD */
                                new_entry->method = method_from_string(begin);
                                if (new_entry->port == NULL) {
                                        new_entry->port = default_port(new_entry->method);
                                }
                        } else if (strncmp(buffer, "emulation", strlen("emulation")) == 0) {
                                /* EMULATION */
                                new_entry->emulation = emulation_from_string(begin);
                                /* Set codepage right now in case it's not declared in the file (upgrade case) */
                                new_entry->codepage = default_codepage(new_entry->emulation);
                        } else if (strncmp(buffer, "codepage", strlen("codepage")) == 0) {
                                /* CODEPAGE */
                                new_entry->codepage = codepage_from_string(begin);
                                if (new_entry->codepage == -1) {
                                        new_entry->codepage = default_codepage(new_entry->emulation);
                                }
                        } else if (strncmp(buffer, "quicklearn", strlen("quicklearn")) == 0) {
                                /* QUICKLEARN */
                                if (strncmp(begin, "true", strlen("true")) == 0) {
                                        new_entry->quicklearn = Q_TRUE;
                                }
#ifndef Q_NO_SERIAL
                        } else if (strncmp(buffer, "use_modem_cfg", strlen("use_modem_cfg")) == 0) {
                                /* USE_MODEM_CFG */
                                if (strncmp(begin, "false", strlen("false")) == 0) {
                                        new_entry->use_modem_cfg = Q_FALSE;
                                }
#endif /* Q_NO_SERIAL */
                        } else if (strncmp(buffer, "use_default_toggles", strlen("use_default_toggles")) == 0) {
                                /* USE_DEFAULT_TOGGLES */
                                if (strncmp(begin, "false", strlen("false")) == 0) {
                                        new_entry->use_default_toggles = Q_FALSE;
                                }
                        } else if (strncmp(buffer, "toggles", strlen("toggles")) == 0) {
                                /* TOGGLES */
                                new_entry->toggles = atoi(begin);
#ifndef Q_NO_SERIAL
                        } else if (strncmp(buffer, "xonxoff", strlen("xonxoff")) == 0) {
                                /* XONXOFF */
                                if (strncmp(begin, "true", strlen("true")) == 0) {
                                        new_entry->xonxoff = Q_TRUE;
                                }
                        } else if (strncmp(buffer, "rtscts", strlen("rtscts")) == 0) {
                                /* RTSCTS */
                                if (strncmp(begin, "false", strlen("false")) == 0) {
                                        new_entry->rtscts = Q_FALSE;
                                }
                        } else if (strncmp(buffer, "baud", strlen("baud")) == 0) {
                                /* BAUD */
                                if (strcmp(begin, "300") == 0) {
                                        new_entry->baud = Q_BAUD_300;
                                } else if (strcmp(begin, "1200") == 0) {
                                        new_entry->baud = Q_BAUD_1200;
                                } else if (strcmp(begin, "2400") == 0) {
                                        new_entry->baud = Q_BAUD_2400;
                                } else if (strcmp(begin, "4800") == 0) {
                                        new_entry->baud = Q_BAUD_4800;
                                } else if (strcmp(begin, "9600") == 0) {
                                        new_entry->baud = Q_BAUD_9600;
                                } else if (strcmp(begin, "19200") == 0) {
                                        new_entry->baud = Q_BAUD_19200;
                                } else if (strcmp(begin, "38400") == 0) {
                                        new_entry->baud = Q_BAUD_38400;
                                } else if (strcmp(begin, "57600") == 0) {
                                        new_entry->baud = Q_BAUD_57600;
                                } else if (strcmp(begin, "115200") == 0) {
                                        new_entry->baud = Q_BAUD_115200;
                                } else if (strcmp(begin, "230400") == 0) {
                                        new_entry->baud = Q_BAUD_230400;
                                }

                        } else if (strncmp(buffer, "data_bits", strlen("data_bits")) == 0) {
                                if (strcmp(begin, "8") == 0) {
                                        new_entry->data_bits = DATA_BITS_8;
                                } else if (strcmp(begin, "7") == 0) {
                                        new_entry->data_bits = DATA_BITS_7;
                                } else if (strcmp(begin, "6") == 0) {
                                        new_entry->data_bits = DATA_BITS_6;
                                } else if (strcmp(begin, "5") == 0) {
                                        new_entry->data_bits = DATA_BITS_5;
                                }
                        } else if (strncmp(buffer, "parity", strlen("parity")) == 0) {
                                if (strcmp(begin, "none") == 0) {
                                        new_entry->parity = Q_PARITY_NONE;
                                } else if (strcmp(begin, "even") == 0) {
                                        new_entry->parity = Q_PARITY_EVEN;
                                } else if (strcmp(begin, "odd") == 0) {
                                        new_entry->parity = Q_PARITY_ODD;
                                } else if (strcmp(begin, "mark") == 0) {
                                        new_entry->parity = Q_PARITY_MARK;
                                } else if (strcmp(begin, "space") == 0) {
                                        new_entry->parity = Q_PARITY_SPACE;
                                }
                        } else if (strncmp(buffer, "stop_bits", strlen("stop_bits")) == 0) {
                                if (strcmp(begin, "1") == 0) {
                                        new_entry->stop_bits = STOP_BITS_1;
                                } else if (strcmp(begin, "2") == 0) {
                                        new_entry->stop_bits = STOP_BITS_2;
                                }
                        } else if (strncmp(buffer, "lock_dte_baud", strlen("lock_dte_baud")) == 0) {
                                if (strcmp(begin, "true") == 0) {
                                        new_entry->lock_dte_baud = Q_TRUE;
                                } else if (strcmp(begin, "false") == 0) {
                                        new_entry->lock_dte_baud = Q_FALSE;
                                }
#endif /* Q_NO_SERIAL */

                        } else if (strncmp(buffer, "times_on", strlen("times_on")) == 0) {
                                /* TIMES ON */
                                new_entry->times_on = atol(begin);
                        } else if (strncmp(buffer, "last_call", strlen("last_call")) == 0) {
                                /* LAST CALL */
                                new_entry->last_call = atol(begin);
                        } else if (strncmp(buffer, "notes", strlen("notes")) == 0) {
                                /* Switch state to reading Notes */
                                new_entry->notes = (wchar_t **)Xmalloc(sizeof(wchar_t *), __FILE__, __LINE__);
                                notes_length = 0;
                                new_entry->notes[notes_length] = NULL;
                                scan_state = SCAN_STATE_NOTES;
                                continue;
                        } else if (strncmp(buffer, "script_filename", strlen("script_filename")) == 0) {
                                /* SCRIPT FILENAME */
                                new_entry->script_filename = Xstrdup(begin, __FILE__, __LINE__);
                        } else if (strncmp(buffer, "capture_filename", strlen("capture_filename")) == 0) {
                                /* CAPTURE FILENAME */
                                new_entry->capture_filename = Xstrdup(begin, __FILE__, __LINE__);
                        } else if (strncmp(buffer, "keybindings_filename", strlen("keybindings_filename")) == 0) {
                                /* KEY BINDINGS */
                                new_entry->keybindings_filename = Xstrdup(begin, __FILE__, __LINE__);
                                /* Last item, switch state */
                                scan_state = SCAN_STATE_NONE;
                        }
                } /* (scan_state == SCAN_STATE_ENTRY) */

                if (scan_state == SCAN_STATE_NOTES) {
                        while ((strlen(line) > 0) && isspace(line[strlen(line)-1])) {
                                /* Trim trailing whitespace */
                                line[strlen(line)-1] = '\0';
                        }
                        if ((strlen(line) > 0) && (strncmp(line, "END", strlen(line)) == 0)) {
                                /* Done, back to entry scanning */
                                scan_state = SCAN_STATE_ENTRY;
                                continue;
                        }
                        notes_length++;
                        new_entry->notes = (wchar_t **)Xrealloc(new_entry->notes, (notes_length + 1) * sizeof(wchar_t *), __FILE__, __LINE__);
                        /* Ensure line is terminated */
                        line[sizeof(line) - 1] = 0;
                        mbstowcs(value_wchar, line, strlen(line) + 1);
                        new_entry->notes[notes_length-1] = Xwcsdup(value_wchar, __FILE__, __LINE__);
                        new_entry->notes[notes_length] = NULL;
                }

        } /* while (!feof(file)) */

        /* Fixup any missing fields */
        if (new_entry->script_filename == NULL) {
                new_entry->script_filename = Xstrdup("", __FILE__, __LINE__);
        }
        if (new_entry->capture_filename == NULL) {
                new_entry->capture_filename = Xstrdup("", __FILE__, __LINE__);
        }
        if (new_entry->keybindings_filename == NULL) {
                new_entry->keybindings_filename = Xstrdup("", __FILE__, __LINE__);
        }

        q_phonebook.selected_entry = q_phonebook.entries;
        fclose(file);
        phonebook_page = 0;
        phonebook_entry_i = 0;
} /* ---------------------------------------------------------------------- */

/*
 * save_phonebook
 */
static void save_phonebook(const Q_BOOL backup_version) {
        FILE * file;
        char * filename;
        struct q_phone_struct * entry;
        char notify_message[DIALOG_MESSAGE_SIZE];
        wchar_t * notes_line;
        int current_notes_idx;

        if (backup_version == Q_FALSE) {
                filename = q_phonebook.filename;
        } else {
                filename = (char *)Xmalloc(sizeof(char) * (strlen(q_phonebook.filename) + 5), __FILE__, __LINE__);
                snprintf(filename, strlen(q_phonebook.filename) + 5, "%s.bak", q_phonebook.filename);
        }

        file = fopen(filename, "w");
        if (file == NULL) {
                snprintf(notify_message, sizeof(notify_message), _("Error opening file \"%s\" for writing: %s"), filename, strerror(errno));
                notify_form(notify_message, 0);
                if (backup_version == Q_TRUE) {
                        Xfree(filename, __FILE__, __LINE__);
                }
                return;
        }

        fprintf(file, "# Qodem Phonebook\n");
        fprintf(file, "#\n");

        for (entry = q_phonebook.entries; entry!=NULL; entry=entry->next) {
                fprintf(file, "[entry]\n");
                fprintf(file, "name=%ls\n", entry->name);
                fprintf(file, "address=%s\n", entry->address);
                fprintf(file, "port=%s\n", entry->port);
                fprintf(file, "username=%ls\n", entry->username);
                fprintf(file, "password=%ls\n", entry->password);
                fprintf(file, "tagged=%s\n", (entry->tagged == Q_TRUE) ? "true" : "false");
                switch (entry->doorway) {
                case Q_DOORWAY_ALWAYS_DOORWAY:
                        fprintf(file, "doorway=doorway\n");
                        break;
                case Q_DOORWAY_ALWAYS_MIXED:
                        fprintf(file, "doorway=mixed\n");
                        break;
                case Q_DOORWAY_NEVER:
                        fprintf(file, "doorway=never\n");
                        break;
                case Q_DOORWAY_CONFIG:
                        fprintf(file, "doorway=config\n");
                        break;
                }
                fprintf(file, "method=%s\n", method_string(entry->method));
                fprintf(file, "emulation=%s\n", emulation_string(entry->emulation));
                fprintf(file, "codepage=%s\n", codepage_string(entry->codepage));
                fprintf(file, "quicklearn=%s\n", (entry->quicklearn == Q_TRUE ? "true" : "false"));
#ifndef Q_NO_SERIAL
                fprintf(file, "use_modem_cfg=%s\n", (entry->use_modem_cfg == Q_TRUE ? "true" : "false"));
                fprintf(file, "baud=%s\n", baud_string(entry->baud));
                fprintf(file, "data_bits=%s\n", data_bits_string(entry->data_bits));
                fprintf(file, "parity=%s\n", parity_string(entry->parity, Q_FALSE));
                fprintf(file, "stop_bits=%s\n", stop_bits_string(entry->stop_bits));
                fprintf(file, "xonxoff=%s\n", (entry->xonxoff == Q_TRUE ? "true" : "false"));
                fprintf(file, "rtscts=%s\n", (entry->rtscts == Q_TRUE ? "true" : "false"));
                fprintf(file, "lock_dte_baud=%s\n", (entry->lock_dte_baud == Q_TRUE ? "true" : "false"));
#endif /* Q_NO_SERIAL */
                fprintf(file, "times_on=%u\n", entry->times_on);
                fprintf(file, "use_default_toggles=%s\n", (entry->use_default_toggles == Q_TRUE ? "true" : "false"));
                fprintf(file, "toggles=%u\n", entry->toggles);
                fprintf(file, "last_call=%lu\n", entry->last_call);
                if (entry->notes != NULL) {
                        fprintf(file, "notes=<<<END\n");
                        current_notes_idx = 0;
                        for (notes_line = entry->notes[current_notes_idx]; notes_line != NULL; current_notes_idx++, notes_line = entry->notes[current_notes_idx]) {
                                fprintf(file, "%ls\n", notes_line);
                        }
                        fprintf(file, "END\n");
                }
                fprintf(file, "script_filename=%s\n", entry->script_filename);
                fprintf(file, "capture_filename=%s\n", entry->capture_filename);
                fprintf(file, "keybindings_filename=%s\n", entry->keybindings_filename);
                fprintf(file, "\n");
        }
        fclose(file);

        if (backup_version == Q_TRUE) {
                Xfree(filename, __FILE__, __LINE__);
        }
} /* ---------------------------------------------------------------------- */

/*
 * create_phonebook
 */
void create_phonebook() {
        /*
         * We will create the following entries:
         *
         * 1) A local shell using L_UTF8 emulation
         * 2) Vertrauen - telnet BBS (home of synchronet)
         * 3) Electronic Chicken - dial-up BBS
         */
        struct q_phone_struct * new_entry = NULL;
        struct q_phone_struct * old_entry = NULL;

        assert(q_phonebook.entries == NULL);

        q_phonebook.entry_count = 0;
        new_entry = (struct q_phone_struct *)Xmalloc(sizeof(struct q_phone_struct), __FILE__, __LINE__);
        if (q_phonebook.entry_count == 0) {
                q_phonebook.entries = new_entry;
                new_entry->prev = NULL;
        } else {
                old_entry->next = new_entry;
                new_entry->prev = old_entry;
        }
        new_entry->name                 = Xstring_to_wcsdup(_("Local shell"), __FILE__, __LINE__);
        new_entry->method               = Q_DIAL_METHOD_SHELL;
        new_entry->address              = Xstrdup("", __FILE__, __LINE__);
        new_entry->port                 = Xstrdup("", __FILE__, __LINE__);
        new_entry->username             = Xwcsdup(L"", __FILE__, __LINE__);
        new_entry->password             = Xwcsdup(L"", __FILE__, __LINE__);
        new_entry->notes                = NULL;
        new_entry->tagged               = Q_FALSE;
        new_entry->doorway              = Q_DOORWAY_CONFIG;
        new_entry->emulation            = Q_EMUL_LINUX_UTF8;
        new_entry->codepage             = default_codepage(new_entry->emulation);
#ifndef Q_NO_SERIAL
        new_entry->use_modem_cfg        = Q_TRUE;
        new_entry->baud                 = Q_BAUD_115200;
        new_entry->data_bits            = DATA_BITS_8;
        new_entry->parity               = Q_PARITY_NONE;
        new_entry->stop_bits            = STOP_BITS_1;
        new_entry->xonxoff              = Q_FALSE;
        new_entry->rtscts               = Q_TRUE;
        new_entry->lock_dte_baud        = Q_TRUE;
#endif /* Q_NO_SERIAL */
        new_entry->script_filename      = Xstrdup("", __FILE__, __LINE__);
        new_entry->capture_filename     = Xstrdup("", __FILE__, __LINE__);
        new_entry->keybindings_filename = Xstrdup("", __FILE__, __LINE__);
        new_entry->use_default_toggles  = Q_TRUE;
        new_entry->toggles              = 0;
        new_entry->last_call            = 0;
        new_entry->times_on             = 0;
        new_entry->quicklearn           = Q_FALSE;
        new_entry->next                 = NULL;
        q_phonebook.entry_count++;

        old_entry = new_entry;
        new_entry = (struct q_phone_struct *)Xmalloc(sizeof(struct q_phone_struct), __FILE__, __LINE__);
        if (q_phonebook.entry_count == 0) {
                q_phonebook.entries = new_entry;
                new_entry->prev = NULL;
        } else {
                old_entry->next = new_entry;
                new_entry->prev = old_entry;
        }
        new_entry->name                 = Xwcsdup(L"Vertrauen BBS", __FILE__, __LINE__);
        new_entry->method               = Q_DIAL_METHOD_TELNET;
        new_entry->address              = Xstrdup("vert.synchro.net", __FILE__, __LINE__);
        new_entry->port                 = Xstrdup("23", __FILE__, __LINE__);
        new_entry->username             = Xwcsdup(L"", __FILE__, __LINE__);
        new_entry->password             = Xwcsdup(L"", __FILE__, __LINE__);
        new_entry->notes                = NULL;
        new_entry->tagged               = Q_FALSE;
        new_entry->doorway              = Q_DOORWAY_CONFIG;
        new_entry->emulation            = Q_EMUL_ANSI;
        new_entry->codepage             = default_codepage(new_entry->emulation);
#ifndef Q_NO_SERIAL
        new_entry->use_modem_cfg        = Q_TRUE;
        new_entry->baud                 = Q_BAUD_115200;
        new_entry->data_bits            = DATA_BITS_8;
        new_entry->parity               = Q_PARITY_NONE;
        new_entry->stop_bits            = STOP_BITS_1;
        new_entry->xonxoff              = Q_FALSE;
        new_entry->rtscts               = Q_TRUE;
        new_entry->lock_dte_baud        = Q_TRUE;
#endif /* Q_NO_SERIAL */
        new_entry->script_filename      = Xstrdup("", __FILE__, __LINE__);
        new_entry->capture_filename     = Xstrdup("", __FILE__, __LINE__);
        new_entry->keybindings_filename = Xstrdup("", __FILE__, __LINE__);
        new_entry->use_default_toggles  = Q_TRUE;
        new_entry->toggles              = 0;
        new_entry->last_call            = 0;
        new_entry->times_on             = 0;
        new_entry->quicklearn           = Q_FALSE;
        new_entry->next                 = NULL;
        q_phonebook.entry_count++;

#ifndef Q_NO_SERIAL
        old_entry = new_entry;
        new_entry = (struct q_phone_struct *)Xmalloc(sizeof(struct q_phone_struct), __FILE__, __LINE__);
        if (q_phonebook.entry_count == 0) {
                q_phonebook.entries = new_entry;
                new_entry->prev = NULL;
        } else {
                old_entry->next = new_entry;
                new_entry->prev = old_entry;
        }
        new_entry->name                 = Xwcsdup(L"Electronic Chicken BBS", __FILE__, __LINE__);
        new_entry->method               = Q_DIAL_METHOD_MODEM;
        new_entry->address              = Xstrdup("1-416-273-7230", __FILE__, __LINE__);
        new_entry->port                 = Xstrdup("", __FILE__, __LINE__);
        new_entry->username             = Xwcsdup(L"", __FILE__, __LINE__);
        new_entry->password             = Xwcsdup(L"", __FILE__, __LINE__);
        new_entry->notes                = NULL;
        new_entry->tagged               = Q_FALSE;
        new_entry->doorway              = Q_DOORWAY_CONFIG;
        new_entry->emulation            = Q_EMUL_ANSI;
        new_entry->codepage             = default_codepage(new_entry->emulation);
        new_entry->use_modem_cfg        = Q_TRUE;
        new_entry->baud                 = Q_BAUD_115200;
        new_entry->data_bits            = DATA_BITS_8;
        new_entry->parity               = Q_PARITY_NONE;
        new_entry->stop_bits            = STOP_BITS_1;
        new_entry->xonxoff              = Q_FALSE;
        new_entry->rtscts               = Q_TRUE;
        new_entry->lock_dte_baud        = Q_TRUE;
        new_entry->script_filename      = Xstrdup("", __FILE__, __LINE__);
        new_entry->capture_filename     = Xstrdup("", __FILE__, __LINE__);
        new_entry->keybindings_filename = Xstrdup("", __FILE__, __LINE__);
        new_entry->use_default_toggles  = Q_TRUE;
        new_entry->toggles              = 0;
        new_entry->last_call            = 0;
        new_entry->times_on             = 0;
        new_entry->quicklearn           = Q_FALSE;
        new_entry->next                 = NULL;
        q_phonebook.entry_count++;
#endif /* Q_NO_SERIAL */

        /* Now save it */
        save_phonebook(Q_FALSE);
} /* ---------------------------------------------------------------------- */

#ifdef Q_LIBSSH2
/* Forward reference for function called in do_dialer() */
static Q_BOOL prompt_ssh_password(const wchar_t * initial_username,
        wchar_t ** returned_username,
        const wchar_t * initial_password,
        wchar_t ** returned_password);
#endif /* Q_LIBSSH2 */

/*
 * do_dialer - call the code in dialer.c to create the connection
 */
void do_dialer() {

        if (q_status.current_username != NULL) {
                Xfree(q_status.current_username, __FILE__, __LINE__);
        }

        if (q_status.current_password != NULL) {
                Xfree(q_status.current_password, __FILE__, __LINE__);
        }


#ifdef Q_LIBSSH2
        wchar_t * ssh_username = NULL;
        wchar_t * ssh_password = NULL;

        /*
         * Make sure for SSH connections we have both the username and
         * password available.
         */
        if (    (q_current_dial_entry->method == Q_DIAL_METHOD_SSH) &&
                (q_status.external_ssh == Q_FALSE) &&
                (       (wcslen(q_current_dial_entry->username) == 0) ||
                        (wcslen(q_current_dial_entry->password) == 0))
        ) {
                /*
                 * This is our internal SSH, and either username or
                 * password is blank.
                 */
                if (prompt_ssh_password(q_current_dial_entry->username,
                                &ssh_username,
                                q_current_dial_entry->password,
                                &ssh_password) == Q_FALSE) {

                        /* The user cancelled */
                        return;
                }

                /* We've got a username/password, use it */
                q_status.current_username = Xwcsdup(ssh_username, __FILE__, __LINE__);
                q_status.current_password = Xwcsdup(ssh_password, __FILE__, __LINE__);
                /* No leak */
                Xfree(ssh_username, __FILE__, __LINE__);
                Xfree(ssh_password, __FILE__, __LINE__);
                ssh_username = NULL;
                ssh_password = NULL;

        } else {
                /*
                 * Either external SSH or some other dial method.
                 */
                q_status.current_username = Xwcsdup(q_current_dial_entry->username, __FILE__, __LINE__);
                q_status.current_password = Xwcsdup(q_current_dial_entry->password, __FILE__, __LINE__);
        }

#else

        /*
         * Either external SSH or some other dial method.
         */
        q_status.current_username = Xwcsdup(q_current_dial_entry->username, __FILE__, __LINE__);
        q_status.current_password = Xwcsdup(q_current_dial_entry->password, __FILE__, __LINE__);

#endif /* Q_LIBSSH2 */

#ifndef Q_NO_SERIAL
        /*
         * Kill the modem.  Either we are switching to a non-modem connection,
         * or we want the terminal settings of the phonebook entry.
         */
        if (q_status.serial_open == Q_TRUE) {
                close_serial_port();
        }
        if (q_current_dial_entry->method == Q_DIAL_METHOD_MODEM) {
                /* Set inital modem_state */
                modem_state = DIAL_MODEM_INIT;
        }
#endif /* Q_NO_SERIAL */
        /* Clear modem message */
        memset(q_dialer_modem_message, 0, sizeof(q_dialer_modem_message));

        if (q_status.remote_address != NULL) {
                Xfree(q_status.remote_address, __FILE__, __LINE__);
        }
        q_status.remote_address = Xstrdup(q_current_dial_entry->address, __FILE__, __LINE__);

        if (q_status.remote_port != NULL) {
                Xfree(q_status.remote_port, __FILE__, __LINE__);
        }
        if ((q_current_dial_entry->port != NULL) &&
                (strlen(q_current_dial_entry->port) > 0)) {
                q_status.remote_port = Xstrdup(q_current_dial_entry->port, __FILE__, __LINE__);
        } else {
                q_status.remote_port = default_port(q_current_dial_entry->method);
        }

        if (q_status.remote_phonebook_name != NULL) {
                Xfree(q_status.remote_phonebook_name, __FILE__, __LINE__);
        }
        q_status.remote_phonebook_name = Xwcsdup(q_current_dial_entry->name, __FILE__, __LINE__);

        /* Save the method - qodem_read() needs it */
        q_status.dial_method = q_current_dial_entry->method;

        /* Save phonebook - in case someone JUST added an entry and will be dialing. */
        save_phonebook(Q_FALSE);

        /* Now do the connection */
        dial_out(q_current_dial_entry);

        /* Switch keyboard */
        switch_current_keyboard(q_current_dial_entry->keybindings_filename);

        /* Capture file */
        if (strlen(q_current_dial_entry->capture_filename) > 0) {
                if (q_status.capture == Q_TRUE) {
                        stop_capture();
                }
                start_capture(q_current_dial_entry->capture_filename);
        }

        /* QuickLearn */
        if (q_current_dial_entry->quicklearn == TRUE) {
                assert(q_current_dial_entry->script_filename != NULL);
                assert(strlen(q_current_dial_entry->script_filename) > 0);
                start_quicklearn(get_scriptdir_filename(q_current_dial_entry->script_filename));
                /* We just dialed out, don't quicklearn again */
                q_current_dial_entry->quicklearn = Q_FALSE;
        }

        /* Save phonebook */
        save_phonebook(Q_FALSE);
} /* ---------------------------------------------------------------------- */

/*
 * sort_phonebook
 */
static void sort_phonebook(const SORT_METHOD method) {
        /*
        This is a hideously inefficient method.  Basically I just keep looping
        through the phonebook swapping adjacent entries until the whole
        thing is sorted.
        */

        struct q_phone_struct * current_entry;
        struct q_phone_struct * swap;
        struct q_phone_struct * tail;
        Q_BOOL sorted = Q_FALSE;

        if (q_phonebook.entries == NULL) {
                return;
        }

        if (method == SORT_METHOD_REVERSE) {
                /* Special case: reverse everything */
                for (current_entry = q_phonebook.entries; current_entry != NULL; current_entry = current_entry->prev) {
                        /* Tail will be the last entry and become the new head */
                        tail = current_entry;

                        /* Switch out prev/next */
                        swap = current_entry->next;
                        current_entry->next = current_entry->prev;
                        current_entry->prev = swap;
                }

                /* Point back to the top */
                q_phonebook.entries = tail;
                q_phonebook.selected_entry = q_phonebook.entries;
                phonebook_page = 0;
                phonebook_entry_i = 0;
                return;
        }

        while (sorted == Q_FALSE) {

                sorted = Q_TRUE;

                for (current_entry=q_phonebook.entries; current_entry->next!=NULL; current_entry=current_entry->next) {

                        if (method == SORT_METHOD_NAME_ASC) {
                                if (wcscmp(current_entry->name, current_entry->next->name) <= 0) {
                                        continue;
                                }

                        } else if (method == SORT_METHOD_ADDRESS_ASC) {
                                if (strcasecmp(current_entry->address, current_entry->next->address) <= 0) {
                                        continue;
                                }

                        } else if (method == SORT_METHOD_TOTAL_CALLS_DESC) {
                                if (current_entry->times_on >= current_entry->next->times_on) {
                                        continue;
                                }

                        } else if (method == SORT_METHOD_METHOD_ASC) {
                                if (current_entry->method <= current_entry->next->method) {
                                        continue;
                                }

                        } else if (method == SORT_METHOD_LAST_CALL_DESC) {
                                if (current_entry->last_call >= current_entry->next->last_call) {
                                        continue;
                                }

                        }

                        /* Swap current_entry and current_entry->next */
                        if (current_entry == q_phonebook.entries) {
                                /* Swapping the head */
                                q_phonebook.entries = current_entry->next;
                                swap = current_entry->next->next;

                                q_phonebook.entries->next = current_entry;
                                q_phonebook.entries->prev = NULL;
                                current_entry->prev = current_entry->next;
                                current_entry->next = swap;
                                if (swap != NULL) {
                                        swap->prev = current_entry;
                                }
                        } else {
                                /* Swapping in the middle */
                                swap = current_entry->next->next;

                                current_entry->prev->next = current_entry->next;
                                current_entry->next->prev = current_entry->prev;
                                current_entry->prev = current_entry->next;
                                current_entry->next->next = current_entry;
                                current_entry->next = swap;
                                if (swap != NULL) {
                                        swap->prev = current_entry;
                                }
                        }

                        sorted = Q_FALSE;

                        /* Search again from the top */
                        break;

                } /* for (...) */

        } /* while (sorted == Q_FALSE) */

        /* Point back to the top */
        q_phonebook.selected_entry = q_phonebook.entries;
        phonebook_page = 0;
        phonebook_entry_i = 0;
} /* ---------------------------------------------------------------------- */

/*
 * match_phonebook_entry.  Note forces search_string to lowercase.
 */
static Q_BOOL match_phonebook_entry(wchar_t * search_string, const struct q_phone_struct * entry) {
        wchar_t * field_string;
        int i, j;

        /* Force the search string to lowercase */
        for (i = 0; i < wcslen(search_string); i++) {
                search_string[i] = towlower(search_string[i]);
        }

        /* Check name */
        if (wcslen(entry->name) > 0) {
                field_string = Xwcsdup(entry->name, __FILE__, __LINE__);
                for (i = 0; i < wcslen(field_string); i++) {
                        field_string[i] = towlower(field_string[i]);
                }

                if (wcsstr(field_string, search_string) != NULL) {
                        Xfree(field_string, __FILE__, __LINE__);
                        return Q_TRUE;
                }
                Xfree(field_string, __FILE__, __LINE__);
        }

        /* Check address */
        if (strlen(entry->address) > 0) {
                field_string = Xstring_to_wcsdup(entry->address, __FILE__, __LINE__);
                for (i = 0; i < wcslen(field_string); i++) {
                        field_string[i] = towlower(field_string[i]);
                }

                if (wcsstr(field_string, search_string) != NULL) {
                        Xfree(field_string, __FILE__, __LINE__);
                        return Q_TRUE;
                }
                Xfree(field_string, __FILE__, __LINE__);
        }

        /* Check notes */
        if (entry->notes == NULL) {
                /* Can't match without notes */
                return Q_FALSE;
        }

        /* Notes is a list of strings */
        for (j=0; entry->notes[j] != NULL; j++) {
                field_string = Xwcsdup(entry->notes[j], __FILE__, __LINE__);
                for (i = 0; i < wcslen(field_string); i++) {
                        field_string[i] = towlower(field_string[i]);
                }

                if (wcsstr(field_string, search_string) != NULL) {
                        Xfree(field_string, __FILE__, __LINE__);
                        found_note_flag = Q_TRUE;
                        return Q_TRUE;
                }
                Xfree(field_string, __FILE__, __LINE__);
        }

        /* Nothing matched */
        return Q_FALSE;
} /* ---------------------------------------------------------------------- */

/*
 * tag_multiple
 */
static void tag_multiple(const char * tag_string) {
        int i;
        int current_entry_i;
        char ** search_tokens;
        char * search_string;
        wchar_t * wcs_search_string;

        struct q_phone_struct * current_entry;
        if (q_phonebook.entries == NULL) {
                return;
        }
        search_tokens = tokenize_command(tag_string);

        current_entry_i = 0;
        for (current_entry=q_phonebook.entries; current_entry!=NULL; current_entry=current_entry->next) {
                current_entry_i++;

                for (i=0; search_tokens[i] != NULL; i++) {

                        if (tolower(search_tokens[i][0]) == 't') {

                                /* Text search */
                                search_string = &search_tokens[i][1];
                                wcs_search_string = Xstring_to_wcsdup(search_string, __FILE__, __LINE__);

                                if (match_phonebook_entry(wcs_search_string, current_entry) == Q_TRUE) {
                                        current_entry->tagged = Q_TRUE;
                                        q_phonebook.tagged++;
                                }
                                /* No leak */
                                Xfree(wcs_search_string, __FILE__, __LINE__);

                        }

                        if (isdigit(search_tokens[i][0])) {
                                /* Entry number selection */
                                if (atoi(search_tokens[i]) == current_entry_i) {
                                        if (current_entry->tagged == Q_FALSE) {
                                                current_entry->tagged = Q_TRUE;
                                                q_phonebook.tagged++;
                                        }
                                }
                        }
                } /* for (i=0; search_tokens[i] != NULL; i++) */

        } /* for (...) */

        /* Free up the array of token pointers */
        Xfree(search_tokens, __FILE__, __LINE__);
} /* ---------------------------------------------------------------------- */

/*
 * edit_attached_note
 */
static void edit_attached_note(struct q_phone_struct * entry) {
        FILE * file;
        wchar_t * notes_line;
        int notes_length;
        char command_line[COMMAND_LINE_SIZE];
        char line[PHONEBOOK_LINE_SIZE];
        char filename[COMMAND_LINE_SIZE];
        char notify_message[DIALOG_MESSAGE_SIZE];
        int current_notes_idx;
        wchar_t line_wchar[PHONEBOOK_LINE_SIZE];

#ifdef __BORLANDC__
        sprintf(filename, "/tmp/~qodem%u_%lu.tmp", GetCurrentProcessId(), time(NULL));
#else
        sprintf(filename, "/tmp/~qodem%u_%lu.tmp", getpid(), time(NULL));
#endif
        file = fopen(filename, "w");
        if (file == NULL) {
                snprintf(notify_message, sizeof(notify_message), _("Error opening file \"%s\" for writing: %s"), filename, strerror(errno));
                notify_form(notify_message, 0);
                return;
        }

        if (entry->notes != NULL) {
                current_notes_idx = 0;
                for (notes_line = entry->notes[current_notes_idx]; notes_line != NULL; current_notes_idx++, notes_line = entry->notes[current_notes_idx]) {
                        fprintf(file, "%ls\n", notes_line);
                }
        }

        fclose(file);

        sprintf(command_line, "%s %s", get_option(Q_OPTION_EDITOR), filename);
        q_cursor_on();
        screen_clear();
        screen_flush();
        spawn_terminal(command_line);
        q_cursor_off();

        file = fopen(filename, "r");
        if (file == NULL) {
                snprintf(notify_message, sizeof(notify_message), _("Error opening file \"%s\" for reading: %s"), filename, strerror(errno));
                notify_form(notify_message, 0);
                return;
        }

        if (entry->notes != NULL) {
                current_notes_idx = 0;
                for (notes_line = entry->notes[current_notes_idx]; notes_line != NULL; current_notes_idx++, notes_line = entry->notes[current_notes_idx]) {
                        Xfree(notes_line, __FILE__, __LINE__);
                }
                Xfree(entry->notes, __FILE__, __LINE__);
        }

        entry->notes = (wchar_t **)Xmalloc(sizeof(wchar_t *), __FILE__, __LINE__);
        notes_length = 0;
        entry->notes[notes_length] = NULL;

        while (!feof(file)) {
                if (fgets(line, sizeof(line), file) == NULL) {
                        /* This should cause the outer while's feof() check to fail */
                        continue;
                }

                while ((strlen(line) > 0) && isspace(line[strlen(line)-1])) {
                        /* Trim trailing whitespace */
                        line[strlen(line)-1] = '\0';
                }
                notes_length++;
                entry->notes = (wchar_t **)Xrealloc(entry->notes,
                        (notes_length + 1) * sizeof(wchar_t *), __FILE__, __LINE__);
                memset(line_wchar, 0, sizeof(line_wchar));
                mbstowcs(line_wchar, line, strlen(line));
                entry->notes[notes_length - 1] = Xwcsdup(line_wchar, __FILE__, __LINE__);
                entry->notes[notes_length] = NULL;
        }

        fclose(file);
        unlink(filename);
} /* ---------------------------------------------------------------------- */

/*
 * delete_phonebook_entry
 */
static void delete_phonebook_entry(struct q_phone_struct * entry) {
        int i;

        if (entry->prev == NULL) {
                /* Deleting the head */
                q_phonebook.entries             = entry->next;
                if (entry->next != NULL) {
                        entry->next->prev       = NULL;
                }
        } else {
                entry->prev->next               = entry->next;
                if (entry->next != NULL) {
                        entry->next->prev       = entry->prev;
                }
        }

        /* No leak */
        Xfree(entry->name, __FILE__, __LINE__);
        Xfree(entry->address, __FILE__, __LINE__);
        Xfree(entry->port, __FILE__, __LINE__);
        Xfree(entry->username, __FILE__, __LINE__);
        Xfree(entry->password, __FILE__, __LINE__);
        if (entry->notes != NULL) {
                for (i=0; entry->notes[i] != NULL; i++) {
                        Xfree(entry->notes[i], __FILE__, __LINE__);
                }
                Xfree(entry->notes, __FILE__, __LINE__);
        }
        Xfree(entry->script_filename, __FILE__, __LINE__);
        Xfree(entry->capture_filename, __FILE__, __LINE__);
        Xfree(entry->keybindings_filename, __FILE__, __LINE__);
        Xfree(entry, __FILE__, __LINE__);
        q_phonebook.entry_count--;
} /* ---------------------------------------------------------------------- */

/*
 * Popup the tag multiple entry box
 */
static char * pick_tag_string() {
        void * pick_window;
        struct field * field;
        struct fieldset * pick_form;
        char * return_string;
        int window_left;
        int window_top;
        int window_height;
        int window_length;
        char * status_string;
        int status_left_stop;
        int field_length;
        char * title;
        int title_left;
        int keystroke;

        window_height = 3;
        window_length = 73;

        /* Window will be centered on the screen */
        window_left = WIDTH - 1 - window_length;
        if (window_left < 0) {
                window_left = 0;
        } else {
                window_left /= 2;
        }
        /* ...but six rows above the status line */
        window_top = HEIGHT - STATUS_HEIGHT - 1 - 6;
        if (window_top < 0) {
                window_top = 0;
        }

        /* Put up the status line */
        screen_put_color_hline_yx(HEIGHT - 1, 0, cp437_chars[HATCH], WIDTH, Q_COLOR_STATUS);

        status_string = _(" Enter Line #'s,  T-Text  ESC/`-Exit ");
        status_left_stop = WIDTH - strlen(status_string);
        if (status_left_stop <= 0) {
                status_left_stop = 0;
        } else {
                status_left_stop /= 2;
        }
        screen_put_color_str_yx(HEIGHT - 1, status_left_stop, status_string, Q_COLOR_STATUS);

        /* Draw the sub-window */
        pick_window = screen_subwin(window_height, window_length, window_top, window_left);
        if (check_subwin_result(pick_window) == Q_FALSE) {
                /* Force a repaint */
                q_screen_dirty = Q_TRUE;
                return NULL;
        }

        field_length = window_length - strlen(_("Numbers to Tag > ")) - 4;

        /* width, toprow, leftcol, fixed, color_active, color_inactive */
        field = field_malloc(field_length, 1, window_length - field_length - 2, Q_FALSE,
                Q_COLOR_PHONEBOOK_FIELD_TEXT, Q_COLOR_WINDOW_FIELD_HIGHLIGHTED);
        pick_form = fieldset_malloc(&field, 1, pick_window);

        /* Draw the box */
        screen_win_draw_box(pick_window, 0, 0, window_length, window_height);

        /* Place the title */
        title = _("Select Entries");
        title_left = window_length - (strlen(title) + 2);
        if (title_left < 0) {
                title_left = 0;
        } else {
                title_left /= 2;
        }
        screen_win_put_color_printf_yx(pick_window, 0, title_left, Q_COLOR_WINDOW_BORDER, " %s ", title);

        /* Place the prompt text */
        screen_win_put_color_str_yx(pick_window, 1, 2, _("Numbers to Tag > "), Q_COLOR_MENU_COMMAND);

        screen_flush();
        fieldset_render(pick_form);

        for (;;) {
                qodem_win_getch(pick_window, &keystroke, NULL, Q_KEYBOARD_DELAY);
                switch (keystroke) {

                case '`':
                case KEY_ESCAPE:
                        /* The abort exit point */
                        fieldset_free(pick_form);
                        screen_delwin(pick_window);
                        /* Force a repaint */
                        q_screen_dirty = Q_TRUE;
                        return NULL;
                case Q_KEY_BACKSPACE:
                case 0x08:
                        fieldset_backspace(pick_form);
                        break;
                case Q_KEY_LEFT:
                        fieldset_left(pick_form);
                        break;
                case Q_KEY_RIGHT:
                        fieldset_right(pick_form);
                        break;
                case Q_KEY_HOME:
                        fieldset_home_char(pick_form);
                        break;
                case Q_KEY_END:
                        fieldset_end_char(pick_form);
                        break;
                case Q_KEY_IC:
                        fieldset_insert_char(pick_form);
                        break;
                case Q_KEY_DC:
                        fieldset_delete_char(pick_form);
                        break;
                case Q_KEY_ENTER:
                case C_CR:
                        /* The OK exit point */
                        return_string = field_get_char_value(field);
                        fieldset_free(pick_form);
                        screen_delwin(pick_window);
                        /* Force a repaint */
                        q_screen_dirty = Q_TRUE;
                        return return_string;
                default:
                        if (!q_key_code_yes(keystroke)) {
                                /* Pass normal keys to form driver */
                                fieldset_keystroke(pick_form, keystroke);
                        }
                        break;

                }
        }

        /* Should never get here. */
        return NULL;
} /* ---------------------------------------------------------------------- */

#ifdef Q_LIBSSH2
static Q_BOOL prompt_ssh_password(const wchar_t * initial_username,
        wchar_t ** returned_username,
        const wchar_t * initial_password,
        wchar_t ** returned_password) {

        void * pick_window;
        struct field ** fields;
        struct fieldset * pick_form;
        int window_left;
        int window_top;
        int window_height;
        int window_length;
        char * status_string;
        int status_left_stop;
        int field_length;
        char * title;
        int title_left;
        int keystroke;
        Q_BOOL old_keyboard_blocks = q_keyboard_blocks;

        window_height = 5;
        window_length = 30;

        /* Window will be centered on the screen */
        window_left = WIDTH - 1 - window_length;
        if (window_left < 0) {
                window_left = 0;
        } else {
                window_left /= 2;
        }
        window_top = (HEIGHT - STATUS_HEIGHT) / 2;
        if (window_top < 0) {
                window_top = 0;
        }

        /* Put up the status line */
        screen_put_color_hline_yx(HEIGHT - 1, 0, cp437_chars[HATCH], WIDTH, Q_COLOR_STATUS);

        status_string = _(" Enter The SSH Logon Username And Password   ESC/`-Exit ");
        status_left_stop = WIDTH - strlen(status_string);
        if (status_left_stop <= 0) {
                status_left_stop = 0;
        } else {
                status_left_stop /= 2;
        }
        screen_put_color_str_yx(HEIGHT - 1, status_left_stop, status_string, Q_COLOR_STATUS);

        /* Draw the sub-window */
        pick_window = screen_subwin(window_height, window_length, window_top, window_left);
        if (check_subwin_result(pick_window) == Q_FALSE) {
                q_cursor_off();
                /* Force a repaint */
                q_screen_dirty = Q_TRUE;
                return Q_FALSE;
        }

        field_length = window_length - strlen(_("Username ")) - 4;

        /* width, toprow, leftcol, fixed, color_active, color_inactive */
        fields = (struct field **)Xmalloc(2 * sizeof(struct field *), __FILE__, __LINE__);
        fields[0] = field_malloc(field_length, 1, window_length - field_length - 2, Q_FALSE,
                Q_COLOR_PHONEBOOK_FIELD_TEXT, Q_COLOR_WINDOW_FIELD_HIGHLIGHTED);
        fields[1] = field_malloc(field_length, 2, window_length - field_length - 2, Q_FALSE,
                Q_COLOR_PHONEBOOK_FIELD_TEXT, Q_COLOR_WINDOW_FIELD_HIGHLIGHTED);
        pick_form = fieldset_malloc(fields, 2, pick_window);

        /* Populate with initial data */
        field_set_value(fields[0], initial_username);
        field_set_value(fields[1], initial_password);

        /* Draw the box */
        screen_win_draw_box(pick_window, 0, 0, window_length, window_height);

        /* Place the title */
        title = _("SSH Logon Credentials");
        title_left = window_length - (strlen(title) + 2);
        if (title_left < 0) {
                title_left = 0;
        } else {
                title_left /= 2;
        }
        screen_win_put_color_printf_yx(pick_window, 0, title_left, Q_COLOR_WINDOW_BORDER, " %s ", title);

        /* Place the prompt text */
        screen_win_put_color_str_yx(pick_window, 1, 2, _("Username "), Q_COLOR_MENU_COMMAND);
        screen_win_put_color_str_yx(pick_window, 2, 2, _("Password "), Q_COLOR_MENU_COMMAND);

        /* We will use the cursor */
        q_cursor_on();
        q_keyboard_blocks = Q_TRUE;

        screen_flush();
        fieldset_render(pick_form);

        for (;;) {
                qodem_win_getch(pick_window, &keystroke, NULL, Q_KEYBOARD_DELAY);
                switch (keystroke) {

                case '`':
                case KEY_ESCAPE:
                        /* The abort exit point */
                        fieldset_free(pick_form);
                        screen_delwin(pick_window);

                        q_cursor_off();

                        /* Force a repaint */
                        q_screen_dirty = Q_TRUE;
                        q_keyboard_blocks = old_keyboard_blocks;
                        return Q_FALSE;
                case Q_KEY_BACKSPACE:
                case 0x08:
                        fieldset_backspace(pick_form);
                        break;
                case Q_KEY_LEFT:
                        fieldset_left(pick_form);
                        break;
                case Q_KEY_RIGHT:
                        fieldset_right(pick_form);
                        break;
                case Q_KEY_HOME:
                        fieldset_home_char(pick_form);
                        break;
                case Q_KEY_END:
                        fieldset_end_char(pick_form);
                        break;
                case Q_KEY_IC:
                        fieldset_insert_char(pick_form);
                        break;
                case Q_KEY_DC:
                        fieldset_delete_char(pick_form);
                        break;
                case Q_KEY_DOWN:
                        fieldset_next_field(pick_form);
                        fieldset_render(pick_form);
                        break;
                case Q_KEY_UP:
                        fieldset_prev_field(pick_form);
                        fieldset_render(pick_form);
                        break;
                case Q_KEY_ENTER:
                case C_CR:
                        /* The OK exit point */
                        *returned_username = field_get_value(fields[0]);
                        *returned_password = field_get_value(fields[1]);
                        if (    (wcslen(*returned_username) == 0) ||
                                (wcslen(*returned_password) == 0)) {
                                /* User pressed enter but field was blank */
                                break;
                        }
                        /* Blank out the password field */
                        field_set_value(fields[1], L"******");
                        fieldset_render(pick_form);
                        screen_flush();

                        fieldset_free(pick_form);
                        screen_delwin(pick_window);

                        q_cursor_off();

                        /* Force a repaint */
                        q_screen_dirty = Q_TRUE;
                        q_keyboard_blocks = old_keyboard_blocks;
                        return Q_TRUE;
                default:
                        if (!q_key_code_yes(keystroke)) {
                                /* Pass normal keys to form driver */
                                fieldset_keystroke(pick_form, keystroke);
                        }
                        break;

                }
        }

        /* Should never get here. */
        assert(1 == 0);
        return Q_FALSE;
} /* ---------------------------------------------------------------------- */
#endif /* Q_LIBSSH2 */

#ifndef Q_NO_SERIAL
/*
 * Popup the manual dial phone number prompt
 */
static char * pick_manual_phone_number() {
        void * pick_window;
        struct field * field;
        struct fieldset * pick_form;
        char * return_string;
        int window_left;
        int window_top;
        int window_height;
        int window_length;
        char * status_string;
        int status_left_stop;
        int field_length;
        char * title;
        int title_left;
        int keystroke;

        window_height = 3;
        window_length = 73;

        /* Window will be centered on the screen */
        window_left = WIDTH - 1 - window_length;
        if (window_left < 0) {
                window_left = 0;
        } else {
                window_left /= 2;
        }
        /* ...but six rows above the status line */
        window_top = HEIGHT - STATUS_HEIGHT - 1 - 6;
        if (window_top < 0) {
                window_top = 0;
        }

        /* Put up the status line */
        screen_put_color_hline_yx(HEIGHT - 1, 0, cp437_chars[HATCH], WIDTH, Q_COLOR_STATUS);

        status_string = _(" Enter The Phone Number To Call   ESC/`-Exit ");
        status_left_stop = WIDTH - strlen(status_string);
        if (status_left_stop <= 0) {
                status_left_stop = 0;
        } else {
                status_left_stop /= 2;
        }
        screen_put_color_str_yx(HEIGHT - 1, status_left_stop, status_string, Q_COLOR_STATUS);

        /* Draw the sub-window */
        pick_window = screen_subwin(window_height, window_length, window_top, window_left);
        if (check_subwin_result(pick_window) == Q_FALSE) {
                /* Force a repaint */
                q_screen_dirty = Q_TRUE;
                return NULL;
        }

        field_length = window_length - strlen(_("Phone number > ")) - 4;

        /* width, toprow, leftcol, fixed, color_active, color_inactive */
        field = field_malloc(field_length, 1, window_length - field_length - 2, Q_FALSE,
                Q_COLOR_PHONEBOOK_FIELD_TEXT, Q_COLOR_WINDOW_FIELD_HIGHLIGHTED);
        pick_form = fieldset_malloc(&field, 1, pick_window);

        /* Draw the box */
        screen_win_draw_box(pick_window, 0, 0, window_length, window_height);

        /* Place the title */
        title = _("Manual Dial");
        title_left = window_length - (strlen(title) + 2);
        if (title_left < 0) {
                title_left = 0;
        } else {
                title_left /= 2;
        }
        screen_win_put_color_printf_yx(pick_window, 0, title_left, Q_COLOR_WINDOW_BORDER, " %s ", title);

        /* Place the prompt text */
        screen_win_put_color_str_yx(pick_window, 1, 2, _("Phone number > "), Q_COLOR_MENU_COMMAND);

        screen_flush();
        fieldset_render(pick_form);

        for (;;) {
                qodem_win_getch(pick_window, &keystroke, NULL, Q_KEYBOARD_DELAY);
                switch (keystroke) {

                case '`':
                case KEY_ESCAPE:
                        /* The abort exit point */
                        fieldset_free(pick_form);
                        screen_delwin(pick_window);
                        /* Force a repaint */
                        q_screen_dirty = Q_TRUE;
                        return NULL;
                case Q_KEY_BACKSPACE:
                case 0x08:
                        fieldset_backspace(pick_form);
                        break;
                case Q_KEY_LEFT:
                        fieldset_left(pick_form);
                        break;
                case Q_KEY_RIGHT:
                        fieldset_right(pick_form);
                        break;
                case Q_KEY_HOME:
                        fieldset_home_char(pick_form);
                        break;
                case Q_KEY_END:
                        fieldset_end_char(pick_form);
                        break;
                case Q_KEY_IC:
                        fieldset_insert_char(pick_form);
                        break;
                case Q_KEY_DC:
                        fieldset_delete_char(pick_form);
                        break;
                case Q_KEY_ENTER:
                case C_CR:
                        /* The OK exit point */
                        return_string = field_get_char_value(field);
                        fieldset_free(pick_form);
                        screen_delwin(pick_window);
                        /* Force a repaint */
                        q_screen_dirty = Q_TRUE;
                        return return_string;
                default:
                        if (!q_key_code_yes(keystroke)) {
                                /* Pass normal keys to form driver */
                                fieldset_keystroke(pick_form, keystroke);
                        }
                        break;

                }
        }

        /* Should never get here. */
        return NULL;
} /* ---------------------------------------------------------------------- */
#endif /* Q_NO_SERIAL */

/*
 * Popup the tag multiple entry box
 */
static char * pick_print_destination() {
        void * pick_window;
        struct field * field;
        struct fieldset * pick_form;
        char * return_string;
        int window_left;
        int window_top;
        int window_height;
        int window_length;
        char * status_string;
        int status_left_stop;
        int field_length;
        char * title;
        int title_left;
        int keystroke;

        window_height = 3;
        window_length = 73;

        /* Window will be centered on the screen */
        window_left = WIDTH - 1 - window_length;
        if (window_left < 0) {
                window_left = 0;
        } else {
                window_left /= 2;
        }
        /* ...but six rows above the status line */
        window_top = HEIGHT - STATUS_HEIGHT - 1 - 6;
        if (window_top < 0) {
                window_top = 0;
        }

        /* Put up the status line */
        screen_put_color_hline_yx(HEIGHT - 1, 0, cp437_chars[HATCH], WIDTH, Q_COLOR_STATUS);

        status_string = _(" Enter The Destination Device Or File Name.   ESC/`-Exit ");
        status_left_stop = WIDTH - strlen(status_string);
        if (status_left_stop <= 0) {
                status_left_stop = 0;
        } else {
                status_left_stop /= 2;
        }
        screen_put_color_str_yx(HEIGHT - 1, status_left_stop, status_string, Q_COLOR_STATUS);

        /* Draw the sub-window */
        pick_window = screen_subwin(window_height, window_length, window_top, window_left);
        if (check_subwin_result(pick_window) == Q_FALSE) {
                /* Force a repaint */
                q_screen_dirty = Q_TRUE;
                return NULL;
        }

        field_length = window_length - strlen(_("Device or File > ")) - 4;

        /* width, toprow, leftcol, fixed, color_active, color_inactive */
        field = field_malloc(field_length, 1, window_length - field_length - 2, Q_FALSE,
                Q_COLOR_PHONEBOOK_FIELD_TEXT, Q_COLOR_WINDOW_FIELD_HIGHLIGHTED);
        pick_form = fieldset_malloc(&field, 1, pick_window);
        field_set_char_value(field, LPR_FILE_NAME);

        /* Draw the box */
        screen_win_draw_box(pick_window, 0, 0, window_length, window_height);

        /* Place the title */
        title = _("Print Phone Book");
        title_left = window_length - (strlen(title) + 2);
        if (title_left < 0) {
                title_left = 0;
        } else {
                title_left /= 2;
        }
        screen_win_put_color_printf_yx(pick_window, 0, title_left, Q_COLOR_WINDOW_BORDER, " %s ", title);

        /* Place the prompt text */
        screen_win_put_color_str_yx(pick_window, 1, 2, _("Device or File > "), Q_COLOR_MENU_COMMAND);

        screen_flush();
        fieldset_render(pick_form);

        for (;;) {
                qodem_win_getch(pick_window, &keystroke, NULL, Q_KEYBOARD_DELAY);
                switch (keystroke) {

                case '`':
                case KEY_ESCAPE:
                        /* The abort exit point */
                        fieldset_free(pick_form);
                        screen_delwin(pick_window);
                        /* Force a repaint */
                        q_screen_dirty = Q_TRUE;
                        return NULL;
                case Q_KEY_BACKSPACE:
                case 0x08:
                        fieldset_backspace(pick_form);
                        break;
                case Q_KEY_LEFT:
                        fieldset_left(pick_form);
                        break;
                case Q_KEY_RIGHT:
                        fieldset_right(pick_form);
                        break;
                case Q_KEY_HOME:
                        fieldset_home_char(pick_form);
                        break;
                case Q_KEY_END:
                        fieldset_end_char(pick_form);
                        break;
                case Q_KEY_IC:
                        fieldset_insert_char(pick_form);
                        break;
                case Q_KEY_DC:
                        fieldset_delete_char(pick_form);
                        break;
                case Q_KEY_ENTER:
                case C_CR:
                        /* The OK exit point */
                        return_string = field_get_char_value(field);
                        fieldset_free(pick_form);
                        screen_delwin(pick_window);
                        /* Force a repaint */
                        q_screen_dirty = Q_TRUE;
                        return return_string;
                default:
                        if (!q_key_code_yes(keystroke)) {
                                /* Pass normal keys to form driver */
                                fieldset_keystroke(pick_form, keystroke);
                        }
                        break;

                }
        }

        /* Should never get here. */
        return NULL;
} /* ---------------------------------------------------------------------- */

/*
 * cycle_redialer_number - switch to next appropriate number to dial
 */
static void cycle_redialer_number() {
        struct q_phone_struct * entry;
        Q_BOOL wrapped_around;

        /*
         * Note this is identical code to phonebook_keyboard_handler()
         * on C_CR / KEY_ENTER.
         * Keep the two functions in sync.
         */

        entry = q_phonebook.selected_entry;
        if (q_phonebook.tagged == 0) {
                /* We're just dialing one number. */
                return;
        }

        wrapped_around = Q_FALSE;
        for (;;) {
                entry = entry->next;
                if (entry == NULL) {
                        /* Wrap around */
                        entry = q_phonebook.entries;
                        wrapped_around = Q_TRUE;
                }

                if (entry->tagged == Q_TRUE) {
                        q_phonebook.selected_entry = entry;
                        phonebook_normalize();
                        return;
                }

                if ((entry == q_phonebook.selected_entry) && (wrapped_around == Q_TRUE)) {
                        /*
                         * We circled around and found no tagged entries, so
                         * break without setting a new selected_entry.
                         * This is the normal behavior when nothing is tagged.
                         */
                        return;
                }
        }
} /* ---------------------------------------------------------------------- */

/*
 * kill_redialer_number - untag this number, then switch to next appropriate
 * number to dial.  Returns false if no numbers remain to dial.
 */
static Q_BOOL kill_redialer_number() {

        /* Untag it */
        if (q_phonebook.selected_entry->tagged == Q_TRUE) {
                q_phonebook.selected_entry->tagged = Q_FALSE;
                q_phonebook.tagged--;
        }

        /* See if anything is tagged */
        if (q_phonebook.tagged == 0) {
                return Q_FALSE;
        }

        /* We can still call */
        cycle_redialer_number();
        return Q_TRUE;
} /* ---------------------------------------------------------------------- */

#ifdef Q_PDCURSES_WIN32

/*
 * _snwprintf() has trouble with "%s" arguments.  Replace those calls
 * with a simple appender.
 */
static void my_swprintf(wchar_t * str, int n, wchar_t * format, char * arg1) {
        int i;
        for (i = 0; (i < strlen(arg1)) && (i < n); i++) {
                str[wcslen(str) + 1] = 0;
                str[wcslen(str)] = arg1[i];
        }
} /* ---------------------------------------------------------------------- */

/*
 * Borland's swprintf and Visual C++'s swprintf have different arguments.
 */
static void my_swprintf2(wchar_t * str, int n, wchar_t * format, int arg1, wchar_t * arg2) {
#ifdef __BORLANDC__
        /* Borland's swprintf() doesn't take a length argument */
        swprintf(str, format, arg1, arg2);
#else
        swprintf(str, n, format, arg1, arg2);
#endif
} /* ---------------------------------------------------------------------- */

/*
 * Borland's swprintf and Visual C++'s swprintf have different arguments.
 */
static void my_swprintf3(wchar_t * str, int n, wchar_t * format, wchar_t * arg1) {
#ifdef __BORLANDC__
        /* Borland's swprintf() doesn't take a length argument */
        swprintf(str, format, arg1);
#else
        swprintf(str, n, format, arg1);
#endif
} /* ---------------------------------------------------------------------- */

/*
 * Borland's swprintf and Visual C++'s swprintf have different arguments.
 */
static void my_swprintf4(wchar_t * str, int n, wchar_t * format, int arg1) {
#ifdef __BORLANDC__
        /* Borland's swprintf() doesn't take a length argument */
        swprintf(str, format, arg1);
#else
        swprintf(str, n, format, arg1);
#endif
} /* ---------------------------------------------------------------------- */

#else

#define my_swprintf swprintf
#define my_swprintf2 swprintf
#define my_swprintf3 swprintf
#define my_swprintf4 swprintf

#endif /* Q_PDCURSES_WIN32 */

/*
 * phonebook_refresh - this is called by both the phonebook AND the dialer
 */
void phonebook_refresh() {
        char * status_string = NULL;
        int status_left_stop;
        char * title;
        int title_left;
        int window_left;
        int window_top;
        int window_height = HEIGHT - 1;
        int window_length = WIDTH;
        int menu_top, menu_left, menu_title_left;
        char * menu_title = NULL;
        wchar_t entry_buffer[Q_MAX_LINE_LENGTH];
        char * password_stars_buffer;
        int i, j;
        struct q_phone_struct * entry;
        char time_string[TIME_STRING_LENGTH];
        int visible_entries_n = HEIGHT - 1 - 14;
        time_t now;
        int indent = (WIDTH - 80)/2;

        if (q_screen_dirty == Q_FALSE) {
                return;
        }

        /*
        I don't like this but it appears necessary to fix issues under Konsole.
        screen_clear();
        */

        /* Window will be centered on the screen */
        window_left = WIDTH - 1 - window_length;
        if (window_left < 0) {
                window_left = 0;
        } else {
                window_left /= 2;
        }
        window_top = HEIGHT - 1 - window_height;
        if (window_top < 0) {
                window_top = 0;
        } else {
                window_top /= 2;
        }

        /* Draw the sub-window */
        screen_draw_box(window_left, window_top, window_left + window_length, window_top + window_height);

        /* Put up the status line */
        screen_put_color_hline_yx(HEIGHT - 1, 0, cp437_chars[HATCH], WIDTH, Q_COLOR_STATUS);

        if (q_program_state == Q_STATE_PHONEBOOK) {
                status_string = _(",PgUp/Dn-Move Scroll Bar   ENTER-Dial   ESC/`-Exit ");
        } else if (q_program_state == Q_STATE_DIALER) {
                status_string = _(" C-Cycle   K-Kill   X-eXtend Timer   ESC/`-Exit ");
        }

        status_left_stop = WIDTH - (strlen(status_string) + 3);
        if (status_left_stop <= 0) {
                status_left_stop = 0;
        } else {
                status_left_stop /= 2;
        }
        if (q_program_state == Q_STATE_PHONEBOOK) {
                screen_put_color_char_yx(HEIGHT - 1, status_left_stop, ' ', Q_COLOR_STATUS);
                screen_put_color_char_yx(HEIGHT - 1, status_left_stop + 1, cp437_chars[UPARROW], Q_COLOR_STATUS);
                screen_put_color_char_yx(HEIGHT - 1, status_left_stop + 2, cp437_chars[DOWNARROW], Q_COLOR_STATUS);
                screen_put_color_str_yx(HEIGHT - 1, status_left_stop + 3, status_string, Q_COLOR_STATUS);
        } else if (q_program_state == Q_STATE_DIALER) {
                screen_put_color_str_yx(HEIGHT - 1, status_left_stop, status_string, Q_COLOR_STATUS);
        }

        /* Place the "F1 Help" text */
        screen_put_color_str_yx(window_top + window_height - 1, window_left + window_length - 11, _("F1 Help"), Q_COLOR_WINDOW_BORDER);

        /* Place the title */
        title = _("Phone Book");
        title_left = window_length - (strlen(title) + 2);
        if (title_left < 0) {
                title_left = 0;
        } else {
                title_left /= 2;
        }
        screen_put_color_printf_yx(window_top + 0, window_left + title_left, Q_COLOR_WINDOW_BORDER, " %s ", title);

        /* Phone Entry List */
        screen_put_color_printf_yx(window_top + 1, indent + window_left + 2, Q_COLOR_MENU_TEXT, _("FON FILE : %s"), q_phonebook.filename);
        screen_put_color_str_yx(window_top + 2, indent + window_left + 2, _("Total Tags > "), Q_COLOR_MENU_TEXT);
        screen_put_color_printf(Q_COLOR_MENU_COMMAND, "%-u", q_phonebook.tagged);
        screen_put_color_str_yx(window_top + 3, indent + window_left + 1, _("[D]   NAME"), Q_COLOR_MENU_COMMAND);

        if (q_phonebook.view_mode == 0) {
#ifndef Q_NO_SERIAL
                screen_put_color_str_yx(window_top + 3, window_left + 38 + indent, _("ADDRESS/COMMAND/NUMBER"), Q_COLOR_MENU_COMMAND);
#else
                screen_put_color_str_yx(window_top + 3, window_left + 38 + indent, _("ADDRESS/COMMAND"), Q_COLOR_MENU_COMMAND);
#endif /* Q_NO_SERIAL */
                screen_put_color_str_yx(window_top + 3, window_left + 62 + indent, _("METHOD"), Q_COLOR_MENU_COMMAND);
                screen_put_color_str_yx(window_top + 3, window_left + 70 + indent, _("EMULATION"), Q_COLOR_MENU_COMMAND);
        } else if (q_phonebook.view_mode == 1) {
                screen_put_color_str_yx(window_top + 3, window_left + 38 + indent, _("USERNAME"), Q_COLOR_MENU_COMMAND);
                screen_put_color_str_yx(window_top + 3, window_left + 62 + indent, _("PASSWORD"), Q_COLOR_MENU_COMMAND);
        } else if (q_phonebook.view_mode == 2) {
                screen_put_color_str_yx(window_top + 3, window_left + 38 + indent, _("CODEPAGE"), Q_COLOR_MENU_COMMAND);
                screen_put_color_str_yx(window_top + 3, window_left + 47 + indent, _("DOORWAY"), Q_COLOR_MENU_COMMAND);
        } else if (q_phonebook.view_mode == 3) {
                screen_put_color_str_yx(window_top + 3, window_left + 38 + indent, _("TOGGLES"), Q_COLOR_MENU_COMMAND);
                screen_put_color_str_yx(window_top + 3, window_left + 58 + indent, _("SCRIPT"), Q_COLOR_MENU_COMMAND);
        } else if (q_phonebook.view_mode == 4) {
#ifndef Q_NO_SERIAL
                screen_put_color_str_yx(window_top + 3, window_left + 42 + indent, _("SERIAL"), Q_COLOR_MENU_COMMAND);
#endif /* Q_NO_SERIAL */
                screen_put_color_str_yx(window_top + 3, window_left + 51 + indent, _("TIMES ON"), Q_COLOR_MENU_COMMAND);
                screen_put_color_str_yx(window_top + 3, window_left + 62 + indent, _("LAST CALL"), Q_COLOR_MENU_COMMAND);
        }

        /* Phone Entry List */
        /* Push entry to the first entry on this page */
        entry = q_phonebook.entries;
        for (i=0; i<(phonebook_entry_i / visible_entries_n) * visible_entries_n; i++) {
                entry = entry->next;
        }

        for (i=0; (i<visible_entries_n) && (entry != NULL); i++, entry = entry->next) {
                wchar_t flag_tagged;
                wchar_t flag_notes;
                int color;

                if ((entry == q_phonebook.selected_entry) && (entry->tagged == Q_TRUE)) {
                        color = Q_COLOR_PHONEBOOK_SELECTED_TAGGED;
                } else if (entry == q_phonebook.selected_entry) {
                        color = Q_COLOR_PHONEBOOK_SELECTED;
                } else if (entry->tagged == Q_TRUE) {
                        color = Q_COLOR_PHONEBOOK_TAGGED;
                } else {
                        color = Q_COLOR_PHONEBOOK_ENTRY;
                }
                if (entry->tagged == Q_TRUE) {
                        if (entry->quicklearn == Q_TRUE) {
                                flag_tagged = 'Q';
                        } else {
                                flag_tagged = cp437_chars[CHECK];
                        }
                } else {
                        flag_tagged = ' ';
                }
                if (entry->notes != NULL) {
                        flag_notes = cp437_chars[TRIPLET];
                } else {
                        flag_notes = ' ';
                }
                memset(entry_buffer, 0, sizeof(entry_buffer));

                /* Center on screen */
                for (j = 0; j < indent; j++) {
                        entry_buffer[j] = ' ';
                }

                /* NAME */
                my_swprintf2(entry_buffer + wcslen(entry_buffer),
                        Q_MAX_LINE_LENGTH - wcslen(entry_buffer),
                        L"%-3u   %ls",
                        (i + 1 + (visible_entries_n * phonebook_page)), entry->name);
                if (wcslen(entry_buffer) >= 36 + indent) {
                        entry_buffer[36 + indent] = ' ';
                        entry_buffer[37 + indent] = '\0';
                } else if (wcslen(entry_buffer) < 36 + indent) {
                        for (j = wcslen(entry_buffer); j < 37 + indent; j++) {
                                entry_buffer[j] = ' ';
                        }
                        entry_buffer[37 + indent] = '\0';
                }

                if (q_phonebook.view_mode == 0) {
                        /* ADDRESS */
                        my_swprintf(entry_buffer + wcslen(entry_buffer),
                                Q_MAX_LINE_LENGTH - wcslen(entry_buffer),
                                L"%s", entry->address);
                        if (wcslen(entry_buffer) >= 60 + indent) {
                                entry_buffer[60 + indent] = ' ';
                                entry_buffer[61 + indent] = '\0';
                        } else if (wcslen(entry_buffer) < 60 + indent) {
                                for (j = wcslen(entry_buffer); j < 61 + indent; j++) {
                                        entry_buffer[j] = ' ';
                                }
                                entry_buffer[61 + indent] = '\0';
                        }

                        /* METHOD */
                        my_swprintf(entry_buffer + wcslen(entry_buffer),
                                Q_MAX_LINE_LENGTH - wcslen(entry_buffer),
                                L"%s", method_string(entry->method));
                        if (wcslen(entry_buffer) >= 68 + indent) {
                                entry_buffer[68 + indent] = ' ';
                                entry_buffer[69 + indent] = '\0';
                        } else if (wcslen(entry_buffer) < 68 + indent) {
                                for (j = wcslen(entry_buffer); j < 69 + indent; j++) {
                                        entry_buffer[j] = ' ';
                                }
                                entry_buffer[69 + indent] = '\0';
                        }

                        /* EMULATION */
                        my_swprintf(entry_buffer + wcslen(entry_buffer),
                                Q_MAX_LINE_LENGTH - wcslen(entry_buffer),
                                L"%s", emulation_string(entry->emulation));

                } else if (q_phonebook.view_mode == 1) {
                        /* USERNAME */
                        my_swprintf3(entry_buffer + wcslen(entry_buffer),
                                Q_MAX_LINE_LENGTH - wcslen(entry_buffer),
                                L"%ls", entry->username);
                        if (wcslen(entry_buffer) >= 60 + indent) {
                                entry_buffer[60 + indent] = ' ';
                                entry_buffer[61 + indent] = '\0';
                        } else if (wcslen(entry_buffer) < 60 + indent) {
                                for (j = wcslen(entry_buffer); j < 61 + indent; j++) {
                                        entry_buffer[j] = ' ';
                                }
                                entry_buffer[61 + indent] = '\0';
                        }

                        /* PASSWORD */
                        /* Show it only as stars */
                        password_stars_buffer = (char *)Xmalloc(wcslen(entry->password) + 1, __FILE__, __LINE__);
                        memset(password_stars_buffer, 0, wcslen(entry->password) + 1);
                        for (j=0; j<wcslen(entry->password); j++) {
                                password_stars_buffer[j] = '*';
                        }
                        my_swprintf(entry_buffer + wcslen(entry_buffer),
                                Q_MAX_LINE_LENGTH - wcslen(entry_buffer),
                                L"%s", password_stars_buffer);
                        Xfree(password_stars_buffer, __FILE__, __LINE__);

                } else if (q_phonebook.view_mode == 2) {
                        /* CODEPAGE */
                        my_swprintf(entry_buffer + wcslen(entry_buffer),
                                Q_MAX_LINE_LENGTH - wcslen(entry_buffer),
                                L"%s", codepage_string(entry->codepage));
                        if (wcslen(entry_buffer) >= 45 + indent) {
                                entry_buffer[45 + indent] = ' ';
                                entry_buffer[46 + indent] = '\0';
                        } else if (wcslen(entry_buffer) < 45 + indent) {
                                for (j = wcslen(entry_buffer); j < 46 + indent; j++) {
                                        entry_buffer[j] = ' ';
                                }
                                entry_buffer[46 + indent] = '\0';
                        }

                        /* DOORWAY */
                        my_swprintf(entry_buffer + wcslen(entry_buffer),
                                /* Q_MAX_LINE_LENGTH - wcslen(entry_buffer), */
                                WIDTH - 2 - wcslen(entry_buffer),
                                L"%s", doorway_string(entry->doorway));

                } else if (q_phonebook.view_mode == 3) {

                        /* TOGGLES */
                        if (entry->use_default_toggles == Q_FALSE) {
                                my_swprintf(entry_buffer + wcslen(entry_buffer),
                                        Q_MAX_LINE_LENGTH - wcslen(entry_buffer),
                                        L"%s", toggles_to_string(entry->toggles));
                        } else {
                                my_swprintf(entry_buffer + wcslen(entry_buffer),
                                        Q_MAX_LINE_LENGTH - wcslen(entry_buffer),
                                        L"%s", _("Defaults"));
                        }

                        if (wcslen(entry_buffer) >= 56 + indent) {
                                entry_buffer[56 + indent] = ' ';
                                entry_buffer[57 + indent] = '\0';
                        } else if (wcslen(entry_buffer) < 56 + indent) {
                                for (j = wcslen(entry_buffer); j < 57 + indent; j++) {
                                        entry_buffer[j] = ' ';
                                }
                                entry_buffer[57 + indent] = '\0';
                        }

                        /* SCRIPT */
                        my_swprintf(entry_buffer + wcslen(entry_buffer),
                                /* Q_MAX_LINE_LENGTH - wcslen(entry_buffer), */
                                WIDTH - 2 - wcslen(entry_buffer),
                                L"%s", entry->script_filename);

                } else if (q_phonebook.view_mode == 4) {
#ifndef Q_NO_SERIAL
                        /* PORT SETTINGS */
                        if (entry->use_modem_cfg == Q_TRUE) {
                                my_swprintf(entry_buffer + wcslen(entry_buffer),
                                        Q_MAX_LINE_LENGTH - wcslen(entry_buffer),
                                        L"%s", _(" Modem Cfg"));
                        } else {
                                my_swprintf(entry_buffer + wcslen(entry_buffer),
                                        Q_MAX_LINE_LENGTH - wcslen(entry_buffer),
                                        L"%6s",
                                        baud_string(entry->baud));
                                swprintf(entry_buffer + wcslen(entry_buffer),
                                        Q_MAX_LINE_LENGTH - wcslen(entry_buffer),
                                        L" ");
                                my_swprintf(entry_buffer + wcslen(entry_buffer),
                                        Q_MAX_LINE_LENGTH - wcslen(entry_buffer),
                                        L"%s",
                                        data_bits_string(entry->data_bits));
                                my_swprintf(entry_buffer + wcslen(entry_buffer),
                                        Q_MAX_LINE_LENGTH - wcslen(entry_buffer),
                                        L"%s",
                                        parity_string(entry->parity, Q_TRUE));
                                my_swprintf(entry_buffer + wcslen(entry_buffer),
                                        Q_MAX_LINE_LENGTH - wcslen(entry_buffer),
                                        L"%s",
                                        stop_bits_string(entry->stop_bits));
                        }
#endif /* Q_NO_SERIAL */

                        if (wcslen(entry_buffer) >= 53 + indent) {
                                entry_buffer[52 + indent] = ' ';
                                entry_buffer[53 + indent] = '\0';
                        } else if (wcslen(entry_buffer) < 52 + indent) {
                                for (j = wcslen(entry_buffer); j < 53 + indent; j++) {
                                        entry_buffer[j] = ' ';
                                }
                                entry_buffer[53 + indent] = '\0';
                        }

                        /* TIMES ON */
                        if (entry->times_on > 0) {
                                my_swprintf4(entry_buffer + wcslen(entry_buffer),
                                        Q_MAX_LINE_LENGTH - wcslen(entry_buffer),
                                        L"%5u", entry->times_on);
                                if (wcslen(entry_buffer) >= 60 + indent) {
                                        entry_buffer[60 + indent] = ' ';
                                        entry_buffer[61 + indent] = '\0';
                                } else if (wcslen(entry_buffer) < 60 + indent) {
                                        for (j = wcslen(entry_buffer); j < 61 + indent; j++) {
                                                entry_buffer[j] = ' ';
                                        }
                                        entry_buffer[61 + indent] = '\0';
                                }

                                /* LAST CALL */
                                strftime(time_string, sizeof(time_string),
                                        "%a, %d %b %Y %H:%M:%S %z", localtime(&entry->last_call));
                                my_swprintf(entry_buffer + wcslen(entry_buffer),
                                        /* Q_MAX_LINE_LENGTH - wcslen(entry_buffer), */
                                        WIDTH - 2 - wcslen(entry_buffer),
                                        L"%s", time_string);
                        }
                }

                screen_put_color_wcs_yx(window_top + 4 + i, window_left + 1, entry_buffer, color);

                screen_put_color_char_yx(window_top + 4 + i, window_left + 4 + indent,
                        flag_notes, color);
                screen_put_color_char_yx(window_top + 4 + i, window_left + 5 + indent,
                        flag_tagged, color);


                /* Pad out to the end */
                screen_put_color_hline_yx(window_top + 4 + i, window_left + 1 + wcslen(entry_buffer), ' ',
                        window_length - wcslen(entry_buffer) - 2, color);
        }

        /* Bottom menu */
        menu_left = window_left + 1;
        menu_top = window_top + window_height - 9;
        screen_put_color_char_yx(menu_top - 1, menu_left - 1, cp437_chars[Q_WINDOW_LEFT_TEE], Q_COLOR_WINDOW_BORDER);
        screen_put_color_char_yx(menu_top - 1, menu_left - 1 + window_length - 1, cp437_chars[Q_WINDOW_RIGHT_TEE], Q_COLOR_WINDOW_BORDER);
        screen_put_color_hline_yx(menu_top - 1, menu_left, cp437_chars[Q_WINDOW_TOP], window_length - 2, Q_COLOR_WINDOW_BORDER);

        if (q_program_state == Q_STATE_PHONEBOOK) {
                menu_title = _("Commands");
        } else if (q_program_state == Q_STATE_DIALER) {
                menu_title = _("Redialer");
        }

        menu_title_left = window_length - (strlen(menu_title) + 2);
        if (menu_title_left < 0) {
                menu_title_left = 0;
        } else {
                menu_title_left /= 2;
        }
        screen_put_color_printf_yx(menu_top - 1, menu_left - 1 + menu_title_left, Q_COLOR_WINDOW_BORDER, " %s ", menu_title);

        menu_left = 1 + indent;

        if (q_program_state == Q_STATE_PHONEBOOK) {

                /* Normal phonebook */
                screen_put_color_str_yx(menu_top + 0, menu_left + 13, _("Entries"), Q_COLOR_MENU_COMMAND);
                screen_put_color_str_yx(menu_top + 1, menu_left + 1, _("      SP"), Q_COLOR_MENU_COMMAND);
                screen_put_color_str(_(" - Tag/Untag"), Q_COLOR_MENU_TEXT);

                screen_put_color_str_yx(menu_top + 2, menu_left + 1, _("   I-Ins"), Q_COLOR_MENU_COMMAND);
                screen_put_color_str(_(" - Insert New Entry"), Q_COLOR_MENU_TEXT);

                screen_put_color_str_yx(menu_top + 3, menu_left + 1, _("^D/D-Del"), Q_COLOR_MENU_COMMAND);
                screen_put_color_str(_(" - Delete Tagged/Bar"), Q_COLOR_MENU_TEXT);

                screen_put_color_str_yx(menu_top + 4, menu_left + 1, "    ^R/R", Q_COLOR_MENU_COMMAND);
                screen_put_color_str(_(" - Revise Tagged/Bar"), Q_COLOR_MENU_TEXT);

                screen_put_color_str_yx(menu_top + 5, menu_left + 1, "       T", Q_COLOR_MENU_COMMAND);
                screen_put_color_str(_(" - Tag Multiple"), Q_COLOR_MENU_TEXT);

                screen_put_color_str_yx(menu_top + 6, menu_left + 1, "       U", Q_COLOR_MENU_COMMAND);
                screen_put_color_str(_(" - Untag all"), Q_COLOR_MENU_TEXT);

                screen_put_color_str_yx(menu_top + 7, menu_left + 1, "       Q", Q_COLOR_MENU_COMMAND);
                screen_put_color_str(_(" - QuickLearn"), Q_COLOR_MENU_TEXT);

#ifndef Q_NO_SERIAL
                screen_put_color_str_yx(menu_top + 0, menu_left + 39, _("Dial"), Q_COLOR_MENU_COMMAND);
                screen_put_color_str_yx(menu_top + 1, menu_left + 35, "M", Q_COLOR_MENU_COMMAND);
                screen_put_color_str(_(" - Manual Dial"), Q_COLOR_MENU_TEXT);
#endif /* Q_NO_SERIAL */

                screen_put_color_str_yx(menu_top + 3, menu_left + 39, _("Edit"), Q_COLOR_MENU_COMMAND);
                screen_put_color_str_yx(menu_top + 4, menu_left + 35, "N", Q_COLOR_MENU_COMMAND);
                screen_put_color_str(_(" - Attached Note"), Q_COLOR_MENU_TEXT);

                screen_put_color_str_yx(menu_top + 5, menu_left + 35, "V", Q_COLOR_MENU_COMMAND);
                screen_put_color_str(_(" - Linked Script"), Q_COLOR_MENU_TEXT);

                screen_put_color_str_yx(menu_top + 0, menu_left + 62, _("FON"), Q_COLOR_MENU_COMMAND);
                screen_put_color_str_yx(menu_top + 1, menu_left + 58, "F", Q_COLOR_MENU_COMMAND);
                screen_put_color_str(_(" - Find Text"), Q_COLOR_MENU_TEXT);

                screen_put_color_str_yx(menu_top + 2, menu_left + 58, "A", Q_COLOR_MENU_COMMAND);
                screen_put_color_str(_(" - Find Again"), Q_COLOR_MENU_TEXT);

                screen_put_color_str_yx(menu_top + 3, menu_left + 58, "L", Q_COLOR_MENU_COMMAND);
                screen_put_color_str(_(" - Load"), Q_COLOR_MENU_TEXT);

                screen_put_color_str_yx(menu_top + 4, menu_left + 58, "O", Q_COLOR_MENU_COMMAND);
                screen_put_color_str(_(" - Other Info"), Q_COLOR_MENU_TEXT);

                screen_put_color_str_yx(menu_top + 5, menu_left + 55, "^P/P", Q_COLOR_MENU_COMMAND);
                screen_put_color_str(_(" - Print 132/80"), Q_COLOR_MENU_TEXT);

                screen_put_color_str_yx(menu_top + 6, menu_left + 58, "S", Q_COLOR_MENU_COMMAND);
                screen_put_color_str(_(" - Sort"), Q_COLOR_MENU_TEXT);

                screen_put_color_str_yx(menu_top + 7, menu_left + 57, "^U", Q_COLOR_MENU_COMMAND);
                screen_put_color_str(_(" - Undo"), Q_COLOR_MENU_TEXT);

        } else if (q_program_state == Q_STATE_DIALER) {

                /* Redialer screen */


                /* Figure out if we need to change state now */
                switch (q_dial_state) {


                case Q_DIAL_CYCLE:
                        /* Recompute the time remaining */
                        time(&now);
                        q_dialer_cycle_time = 1 - (now - q_dialer_cycle_start_time);
                        if (q_dialer_cycle_time > 0) {
                                break;
                        }

                        /* We're out of time, fall through */
                        q_dial_state = Q_DIAL_BETWEEN_PAUSE;
                        /* Hold this state for 5 seconds */
                        time(&q_dialer_cycle_start_time);

                        /* Fall through ... */
                case Q_DIAL_BETWEEN_PAUSE:
                        /* Recompute the time remaining */
                        time(&now);
                        q_dialer_cycle_time = atoi(get_option(Q_OPTION_DIAL_BETWEEN_TIME)) - (now - q_dialer_cycle_start_time);

                        if (q_dialer_cycle_time > 0) {
                                break;
                        }

                        /* Cycle */
                        cycle_redialer_number();

                        /* Re-do the dial */
                        q_current_dial_entry = q_phonebook.selected_entry;
                        do_dialer();
                        return;

                case Q_DIAL_DIALING:
                        /* Recompute the time remaining */
                        time(&now);
                        q_dialer_cycle_time = atoi(get_option(Q_OPTION_DIAL_CONNECT_TIME)) - (now - q_dialer_cycle_start_time);

                        /* If we're out of time, cycle */
                        if (q_dialer_cycle_time == 0) {
                                q_dial_state = Q_DIAL_CYCLE;
                                /* Hold this state for one second */
                                time(&q_dialer_cycle_start_time);
                                close_dial_entry();
                        }

                        break;

                case Q_DIAL_LINE_BUSY:
                        /* Recompute the time remaining */
                        time(&now);
                        q_dialer_cycle_time = 1 - (now - q_dialer_cycle_start_time);
                        if (q_dialer_cycle_time > 0) {
                                break;
                        }

                        /* We're out of time, fall through */
                        q_dial_state = Q_DIAL_BETWEEN_PAUSE;
                        /* Hold this state for 5 seconds */
                        time(&q_dialer_cycle_start_time);
                        break;

                case Q_DIAL_MANUAL_CYCLE:
                        /* Recompute the time remaining */
                        time(&now);
                        q_dialer_cycle_time = 1 - (now - q_dialer_cycle_start_time);
                        if (q_dialer_cycle_time > 0) {
                                break;
                        }

                        /* We're out of time, fall through */
                        q_dial_state = Q_DIAL_BETWEEN_PAUSE;
                        /* Hold this state for 5 seconds */
                        time(&q_dialer_cycle_start_time);
                        break;

                case Q_DIAL_KILLED:

                        /* Recompute the time remaining */
                        time(&now);
                        q_dialer_cycle_time = 1 - (now - q_dialer_cycle_start_time);
                        if (q_dialer_cycle_time > 0) {
                                break;
                        }

                        /* We're out of time, fall through */
                        q_dial_state = Q_DIAL_BETWEEN_PAUSE;
                        /* Hold this state for 5 seconds */
                        time(&q_dialer_cycle_start_time);
                        break;

                case Q_DIAL_CONNECTED:
                        /* Recompute the time remaining */
                        time(&now);
                        q_dialer_cycle_time = 3 - (now - q_dialer_cycle_start_time);
                        if (q_dialer_cycle_time > 0) {
                                break;
                        }

                        /* Time is up, switch to console */
                        switch_state(Q_STATE_CONSOLE);
                        q_screen_dirty = Q_TRUE;

                        if (    (strlen(q_current_dial_entry->script_filename) > 0) &&
                                (file_exists(get_scriptdir_filename(q_current_dial_entry->script_filename)) == Q_TRUE)) {
                                if (q_status.quicklearn == Q_FALSE) {
                                        /* Execute script if supplied */
                                        script_start(q_current_dial_entry->script_filename);
                                }
                        }
                        break;

                case Q_DIAL_USER_ABORTED:
                case Q_DIAL_NO_NUMBERS_LEFT:

                        /* Recompute the time remaining */
                        time(&now);
                        q_dialer_cycle_time = 1 - (now - q_dialer_cycle_start_time);
                        if (q_dialer_cycle_time > 0) {
                                break;
                        }
                        /* Now return to phonebook */
                        close_dial_entry();
                        q_current_dial_entry = NULL;
                        switch_state(Q_STATE_PHONEBOOK);
                        q_screen_dirty = Q_TRUE;
                        /*
                         * We need to explicitly call refresh_hanlder() because
                         * phonebook_keyboard_handler() blocks.
                         */
                        refresh_handler();
                        return;
                }

                /* Put up the right message */
                switch (q_dial_state) {

                case Q_DIAL_DIALING:
                        /* Set the message */
                        sprintf(q_dialer_status_message, _("%-3d Seconds remain until Cycle"), (int)q_dialer_cycle_time);
                        break;

                case Q_DIAL_CYCLE:
                        /* Set the message */
                        sprintf(q_dialer_status_message, _("Dial timing period expired"));
                        break;

                case Q_DIAL_BETWEEN_PAUSE:
                        /* Set the message */
                        sprintf(q_dialer_status_message, _("Redial pausing %3d"), (int)q_dialer_cycle_time);
                        break;

                case Q_DIAL_LINE_BUSY:
                        /* Set the message */
                        assert(q_current_dial_entry != NULL);
#ifndef Q_NO_SERIAL
                        if (q_current_dial_entry->method == Q_DIAL_METHOD_MODEM) {
                                sprintf(q_dialer_status_message, _("Line busy or modem timed out"));
                        } else {
#endif /* Q_NO_SERIAL */
                                sprintf(q_dialer_status_message, _("Network failed to connect"));
#ifndef Q_NO_SERIAL
                        }
#endif /* Q_NO_SERIAL */
                        break;

                case Q_DIAL_MANUAL_CYCLE:
                        /* Set the message */
                        sprintf(q_dialer_status_message, _("Manual Cycle"));
                        break;

                case Q_DIAL_KILLED:
                        /* Set the message */
                        sprintf(q_dialer_status_message, _("Number has been un-tagged"));
                        break;

                case Q_DIAL_CONNECTED:
                        /* Set the message.  This is duplicated in modem_data(). */
                        sprintf(q_dialer_status_message, _("CONNECTED, press a key to continue"));
                        break;

                case Q_DIAL_NO_NUMBERS_LEFT:
                        /* Set the message */
                        sprintf(q_dialer_status_message, _("No numbers remaining, Dialing aborted"));
                        break;

                case Q_DIAL_USER_ABORTED:
                        /* Set the message */
                        sprintf(q_dialer_status_message, _("[ESC/`] pressed, Dialing aborted"));
                        break;

                }

                assert(q_current_dial_entry != NULL);

                /* Name */
                screen_put_color_str_yx(menu_top + 1, menu_left + 2, _("Name   : "), Q_COLOR_MENU_TEXT);
                screen_put_color_wcs(q_current_dial_entry->name, Q_COLOR_MENU_COMMAND);

#ifndef Q_NO_SERIAL
                if (q_current_dial_entry->method == Q_DIAL_METHOD_MODEM) {
                        /* Number */
                        screen_put_color_str_yx(menu_top + 2, menu_left + 2, _("Number : "), Q_COLOR_MENU_TEXT);
                } else {
#endif /* Q_NO_SERIAL */
                        /* Address */
                        screen_put_color_str_yx(menu_top + 2, menu_left + 2, _("Address: "), Q_COLOR_MENU_TEXT);
#ifndef Q_NO_SERIAL
                }
#endif /* Q_NO_SERIAL */
                screen_put_color_str(q_current_dial_entry->address, Q_COLOR_MENU_COMMAND);

                /* Script */
                screen_put_color_str_yx(menu_top + 3, menu_left + 2, _("Script : "), Q_COLOR_MENU_TEXT);
                screen_put_color_str(q_current_dial_entry->script_filename, Q_COLOR_MENU_COMMAND);

#ifndef Q_NO_SERIAL
                assert(q_current_dial_entry != NULL);
                if (q_current_dial_entry->method == Q_DIAL_METHOD_MODEM) {
                        /* Modem */
                        screen_put_color_str_yx(menu_top + 5, menu_left + 2, _("Modem  : "), Q_COLOR_MENU_TEXT);
                } else {
#endif /* Q_NO_SERIAL */
                        /* Network */
                        screen_put_color_str_yx(menu_top + 5, menu_left + 2, _("Network: "), Q_COLOR_MENU_TEXT);
#ifndef Q_NO_SERIAL
                }
#endif /* Q_NO_SERIAL */
                screen_put_color_str(q_dialer_modem_message, Q_COLOR_MENU_COMMAND);

                /* Status */
                screen_put_color_str_yx(menu_top + 6, menu_left + 2, _("Status : "), Q_COLOR_MENU_TEXT);
                screen_put_color_str(q_dialer_status_message, Q_COLOR_MENU_COMMAND);

                /* Last On */
                screen_put_color_str_yx(menu_top + 1, menu_left + 57, _("Last On : "), Q_COLOR_MENU_TEXT);
                if (q_current_dial_entry->times_on > 0) {
                        strftime(time_string, sizeof(time_string), "%m/%d/%Y", localtime(&q_current_dial_entry->last_call));
                        screen_put_color_str(time_string, Q_COLOR_MENU_COMMAND);
                }

                /* Total # */
                screen_put_color_str_yx(menu_top + 2, menu_left + 57, _("Total # : "), Q_COLOR_MENU_TEXT);
                screen_put_color_printf(Q_COLOR_MENU_COMMAND, "%d", q_current_dial_entry->times_on);

                /* Attempt */
                screen_put_color_str_yx(menu_top + 3, menu_left + 57, _("Attempt : "), Q_COLOR_MENU_TEXT);
                screen_put_color_printf(Q_COLOR_MENU_COMMAND, "%d", q_dialer_attempts);

                /* Start */
                screen_put_color_str_yx(menu_top + 5, menu_left + 57, _("Start   : "), Q_COLOR_MENU_TEXT);
                strftime(time_string, sizeof(time_string), "%H:%M:%S", localtime(&q_dialer_start_time));
                screen_put_color_str(time_string, Q_COLOR_MENU_COMMAND);

                /* Current */
                screen_put_color_str_yx(menu_top + 6, menu_left + 57, _("Current : "), Q_COLOR_MENU_TEXT);
                time(&now);
                strftime(time_string, sizeof(time_string), "%H:%M:%S", localtime(&now));
                screen_put_color_str(time_string, Q_COLOR_MENU_COMMAND);

        }

        /* Push it to the screen */
        screen_flush();

        if (q_program_state == Q_STATE_PHONEBOOK) {
                if (found_note_flag == Q_TRUE) {
                        notify_form(_("Text found in attached Note"), 1.5);
                        found_note_flag = Q_FALSE;
                        /*
                         * Don't clear the dirty flag, we get to do this all
                         * again.
                         */
                        phonebook_refresh();
                } else {
                        /* Other states want dynamic updates */
                        q_screen_dirty = Q_FALSE;
                }
        }

} /* ---------------------------------------------------------------------- */

/*
 * Popup the emulation pick box
 */
static Q_EMULATION pick_emulation() {
        void * pick_window;
        int window_left;
        int window_top;
        int window_height;
        int window_length;
        char * title;
        char selection_buffer[EMULATION_STRING_SIZE];

        int title_left;
        int keystroke;
        int i;
        int selected_field;

        title = _("Emulations");

        window_height = Q_EMULATION_MAX + 2;
        window_length = strlen(title) + 4;

        /* Window will be centered on the screen */
        window_left = WIDTH - 1 - window_length;
        if (window_left < 0) {
                window_left = 0;
        } else {
                window_left /= 2;
        }
        window_top = HEIGHT - 1 - window_height;
        if (window_top < 0) {
                window_top = 0;
        } else {
                window_top /= 2;
        }

        /* Draw the sub-window */
        pick_window = screen_subwin(window_height, window_length, window_top, window_left);
        if (check_subwin_result(pick_window) == Q_FALSE) {
                return -1;
        }

        screen_win_draw_box(pick_window, 0, 0, window_length, window_height);

        /* Place the title */
        title_left = window_length - (strlen(title) + 2);
        if (title_left < 0) {
                title_left = 0;
        } else {
                title_left /= 2;
        }
        screen_win_put_color_printf_yx(pick_window, 0, title_left, Q_COLOR_WINDOW_BORDER, " %s ", title);

        selected_field = 0;

        for (;;) {

                for (i=0; i<Q_EMULATION_MAX; i++) {

                        snprintf(selection_buffer, sizeof(selection_buffer), " %s", emulation_string(i));
                        if (strlen(selection_buffer) < window_length - 3) {
                                memset(selection_buffer + strlen(selection_buffer), ' ', window_length - 2 - strlen(selection_buffer));
                                selection_buffer[window_length - 2] = '\0';
                        }

                        if (selected_field == i) {
                                screen_win_put_color_str_yx(pick_window, i + 1, 1, selection_buffer, Q_COLOR_PHONEBOOK_SELECTED);
                        } else {
                                screen_win_put_color_str_yx(pick_window, i + 1, 1, selection_buffer, Q_COLOR_PHONEBOOK_ENTRY);
                        }
                }

                screen_win_flush(pick_window);
                screen_flush();

                qodem_win_getch(pick_window, &keystroke, NULL, Q_KEYBOARD_DELAY);
                switch (keystroke) {
                case '`':
                case KEY_ESCAPE:
                        /* The abort exit point */
                        screen_delwin(pick_window);
                        return -1;
                case Q_KEY_DOWN:
                        selected_field++;
                        if (selected_field == Q_EMULATION_MAX) {
                                selected_field = 0;
                        }
                        break;
                case Q_KEY_UP:
                        selected_field--;
                        if (selected_field < 0) {
                                selected_field = Q_EMULATION_MAX - 1;
                        }
                        break;
                case Q_KEY_HOME:
                        selected_field = 0;
                        break;
                case Q_KEY_END:
                        selected_field = Q_EMULATION_MAX - 1;
                        break;
                case Q_KEY_ENTER:
                case Q_KEY_F(10):
                case C_CR:
                        /* The OK exit point */
                        screen_delwin(pick_window);
                        return selected_field;
                default:
                        /* Ignore */
                        break;
                }
        }

        /* Should never get here. */
        return -1;
} /* ---------------------------------------------------------------------- */

/*
 * Popup the codepage pick box
 */
static Q_CODEPAGE pick_codepage(Q_EMULATION emulation) {
        void * pick_window;
        int window_left;
        int window_top;
        int window_height;
        int window_length;
        char * title;
        char selection_buffer[CODEPAGE_STRING_SIZE];

        int title_left;
        int keystroke;
        int i;
        int selected_field;

        Q_BOOL codepage_cp437 = Q_FALSE;
        Q_BOOL codepage_iso8859_1 = Q_FALSE;
        Q_BOOL codepage_dec = Q_FALSE;

        /* Only enable a codepage for this emulation */
        switch (emulation) {
        case Q_EMUL_TTY:
                codepage_cp437 = Q_TRUE;
                codepage_iso8859_1 = Q_TRUE;
                break;
        case Q_EMUL_VT52:
        case Q_EMUL_VT100:
        case Q_EMUL_VT102:
        case Q_EMUL_VT220:
        case Q_EMUL_LINUX_UTF8:
        case Q_EMUL_XTERM_UTF8:
                codepage_dec = Q_TRUE;
                break;
        case Q_EMUL_DEBUG:
        case Q_EMUL_ANSI:
        case Q_EMUL_AVATAR:
        case Q_EMUL_LINUX:
        case Q_EMUL_XTERM:
                codepage_cp437 = Q_TRUE;
                codepage_iso8859_1 = Q_TRUE;
                break;
        }

        title = _("Codepages");

        window_height = Q_CODEPAGE_MAX + 2;
        window_length = strlen(codepage_string(Q_CODEPAGE_ISO8859_1)) + 4;

        /* Window will be centered on the screen */
        window_left = WIDTH - 1 - window_length;
        if (window_left < 0) {
                window_left = 0;
        } else {
                window_left /= 2;
        }
        window_top = HEIGHT - 1 - window_height;
        if (window_top < 0) {
                window_top = 0;
        } else {
                window_top /= 2;
        }

        /* Draw the sub-window */
        pick_window = screen_subwin(window_height, window_length, window_top, window_left);
        if (check_subwin_result(pick_window) == Q_FALSE) {
                return -1;
        }

        screen_win_draw_box(pick_window, 0, 0, window_length, window_height);

        /* Place the title */
        title_left = window_length - (strlen(title) + 2);
        if (title_left < 0) {
                title_left = 0;
        } else {
                title_left /= 2;
        }
        screen_win_put_color_printf_yx(pick_window, 0, title_left, Q_COLOR_WINDOW_BORDER, " %s ", title);

        selected_field = 0;

        for (;;) {

                if ((codepage_cp437 == Q_FALSE) && (selected_field == Q_CODEPAGE_CP437)) {
                        selected_field++;
                }
                if ((codepage_iso8859_1 == Q_FALSE) && (selected_field == Q_CODEPAGE_ISO8859_1)) {
                        selected_field++;
                }
                if ((codepage_dec == Q_FALSE) && (selected_field == Q_CODEPAGE_DEC)) {
                        selected_field--;
                }

                for (i = 0; i < Q_CODEPAGE_MAX; i++) {

                        snprintf(selection_buffer, sizeof(selection_buffer), " %s", codepage_string(i));
                        if (strlen(selection_buffer) < window_length - 3) {
                                memset(selection_buffer + strlen(selection_buffer), ' ', window_length - 2 - strlen(selection_buffer));
                                selection_buffer[window_length - 2] = '\0';
                        }

                        if (selected_field == i) {
                                screen_win_put_color_str_yx(pick_window, i + 1, 1, selection_buffer, Q_COLOR_PHONEBOOK_SELECTED);
                        } else {
                                screen_win_put_color_str_yx(pick_window, i + 1, 1, selection_buffer, Q_COLOR_PHONEBOOK_ENTRY);
                        }
                }

                screen_win_flush(pick_window);
                screen_flush();

                qodem_win_getch(pick_window, &keystroke, NULL, Q_KEYBOARD_DELAY);
                switch (keystroke) {
                case '`':
                case KEY_ESCAPE:
                        /* The abort exit point */
                        screen_delwin(pick_window);
                        return -1;
                case Q_KEY_DOWN:
                        if (codepage_dec == Q_FALSE) {
                                selected_field++;
                                /* Skip over DEC */
                                if (selected_field == Q_CODEPAGE_DEC) {
                                        selected_field++;
                                }
                        }
                        if (selected_field == Q_CODEPAGE_MAX) {
                                selected_field--;
                        }
                        break;
                case Q_KEY_UP:
                        if (codepage_dec == Q_FALSE) {
                                selected_field--;
                                /* Skip over DEC */
                                if (selected_field == Q_CODEPAGE_DEC) {
                                        selected_field--;
                                }
                        }
                        if (selected_field < 0) {
                                selected_field++;
                        }
                        break;
                case Q_KEY_HOME:
                        if (codepage_dec == Q_FALSE) {
                                selected_field = 0;
                        }
                        break;
                case Q_KEY_END:
                        if (codepage_dec == Q_FALSE) {
                                selected_field = Q_CODEPAGE_MAX - 1;
                        }
                        break;
                case Q_KEY_ENTER:
                case Q_KEY_F(10):
                case C_CR:
                        /* The OK exit point */
                        screen_delwin(pick_window);
                        return selected_field;
                default:
                        /* Ignore */
                        break;
                }
        }

        /* Should never get here. */
        return -1;
} /* ---------------------------------------------------------------------- */

/*
 * Popup the method pick box
 */
static Q_DIAL_METHOD pick_method() {
        void * pick_window;
        int window_left;
        int window_top;
        int window_height;
        int window_length;
        char * title;
        char selection_buffer[METHOD_STRING_SIZE];

        int title_left;
        int keystroke;
        int i;
        int selected_field;

        title = _("Connection Methods");

        window_height = Q_DIAL_METHOD_MAX + 2;
        window_length = strlen(title) + 4;

        /* Window will be centered on the screen */
        window_left = WIDTH - 1 - window_length;
        if (window_left < 0) {
                window_left = 0;
        } else {
                window_left /= 2;
        }
        window_top = HEIGHT - 1 - window_height;
        if (window_top < 0) {
                window_top = 0;
        } else {
                window_top /= 2;
        }

        /* Draw the sub-window */
        pick_window = screen_subwin(window_height, window_length, window_top, window_left);
        if (check_subwin_result(pick_window) == Q_FALSE) {
                return -1;
        }

        screen_win_draw_box(pick_window, 0, 0, window_length, window_height);

        /* Place the title */
        title_left = window_length - (strlen(title) + 2);
        if (title_left < 0) {
                title_left = 0;
        } else {
                title_left /= 2;
        }
        screen_win_put_color_printf_yx(pick_window, 0, title_left, Q_COLOR_WINDOW_BORDER, " %s ", title);

        selected_field = 0;

        for (;;) {

                for (i=0; i<Q_DIAL_METHOD_MAX; i++) {

                        snprintf(selection_buffer, sizeof(selection_buffer), " %s", method_string(i));
                        if (strlen(selection_buffer) < window_length - 3) {
                                memset(selection_buffer + strlen(selection_buffer), ' ', window_length - 2 - strlen(selection_buffer));
                                selection_buffer[window_length - 2] = '\0';
                        }

                        if (selected_field == i) {
                                screen_win_put_color_str_yx(pick_window, i + 1, 1, selection_buffer, Q_COLOR_PHONEBOOK_SELECTED);
                        } else {
                                screen_win_put_color_str_yx(pick_window, i + 1, 1, selection_buffer, Q_COLOR_PHONEBOOK_ENTRY);
                        }
                }

                screen_win_flush(pick_window);
                screen_flush();

                qodem_win_getch(pick_window, &keystroke, NULL, Q_KEYBOARD_DELAY);
                switch (keystroke) {
                case '`':
                case KEY_ESCAPE:
                        /* The abort exit point */
                        screen_delwin(pick_window);
                        return -1;
                case Q_KEY_DOWN:
                        selected_field++;
                        if (selected_field == Q_DIAL_METHOD_MAX) {
                                selected_field = 0;
                        }
                        break;
                case Q_KEY_UP:
                        selected_field--;
                        if (selected_field < 0) {
                                selected_field = Q_DIAL_METHOD_MAX - 1;
                        }
                        break;
                case Q_KEY_HOME:
                        selected_field = 0;
                        break;
                case Q_KEY_END:
                        selected_field = Q_DIAL_METHOD_MAX - 1;
                        break;
                case Q_KEY_ENTER:
                case Q_KEY_F(10):
                case C_CR:
                        /* The OK exit point */
                        screen_delwin(pick_window);
                        return selected_field;
                default:
                        /* Ignore */
                        break;
                }
        }

        /* Should never get here. */
        return -1;
} /* ---------------------------------------------------------------------- */

/*
 * Popup the sort pick box
 */
static SORT_METHOD pick_sort() {
        void * pick_window;
        int window_left;
        int window_top;
        int window_height;
        int window_length;
        char * title;
        /* This doubles as the window length, so it's hardcoded */
        char selection_buffer[38];

        int title_left;
        int keystroke;
        int i;
        int selected_field;

        char * sort_strings[6];

        sort_strings[0] = _("Name (ascending)");
        sort_strings[1] = _("Number / Address (ascending)");
        sort_strings[2] = _("Total Calls (descending)");
        sort_strings[3] = _("Connection Method (ascending)");
        sort_strings[4] = _("Last Call (descending)");
        sort_strings[5] = _("Reverse All");

        window_height = SORT_METHOD_MAX + 2;
        window_length = sizeof(selection_buffer);

        /* Window will be centered on the screen */
        window_left = WIDTH - 1 - window_length;
        if (window_left < 0) {
                window_left = 0;
        } else {
                window_left /= 2;
        }
        window_top = (HEIGHT - 1 - window_height) * 2;
        if (window_top < 0) {
                window_top = 0;
        } else {
                window_top /= 3;
        }

        /* Draw the sub-window */
        pick_window = screen_subwin(window_height, window_length, window_top, window_left);
        if (check_subwin_result(pick_window) == Q_FALSE) {
                return -1;
        }

        screen_win_draw_box(pick_window, 0, 0, window_length, window_height);

        /* Place the title */
        title = _("Sort FON By:");
        title_left = window_length - (strlen(title) + 2);
        if (title_left < 0) {
                title_left = 0;
        } else {
                title_left /= 2;
        }
        screen_win_put_color_printf_yx(pick_window, 0, title_left, Q_COLOR_WINDOW_BORDER, " %s ", title);

        selected_field = 0;

        for (;;) {

                for (i=0; i<SORT_METHOD_MAX; i++) {
                        snprintf(selection_buffer, sizeof(selection_buffer), " %s", sort_strings[i]);
                        if (strlen(selection_buffer) < window_length - 3) {
                                memset(selection_buffer + strlen(selection_buffer), ' ', window_length - 2 - strlen(selection_buffer));
                                selection_buffer[window_length - 2] = '\0';
                        }

                        if (selected_field == i) {
                                screen_win_put_color_str_yx(pick_window, i + 1, 1, selection_buffer, Q_COLOR_PHONEBOOK_SELECTED);
                        } else {
                                screen_win_put_color_str_yx(pick_window, i + 1, 1, selection_buffer, Q_COLOR_PHONEBOOK_ENTRY);
                        }
                }

                screen_win_flush(pick_window);
                screen_flush();

                qodem_win_getch(pick_window, &keystroke, NULL, Q_KEYBOARD_DELAY);
                switch (keystroke) {
                case '`':
                case KEY_ESCAPE:
                        /* The abort exit point */
                        screen_delwin(pick_window);
                        return -1;
                case Q_KEY_DOWN:
                        selected_field++;
                        if (selected_field == SORT_METHOD_MAX) {
                                selected_field = 0;
                        }
                        break;
                case Q_KEY_UP:
                        selected_field--;
                        if (selected_field < 0) {
                                selected_field = SORT_METHOD_MAX - 1;
                        }
                        break;
                case Q_KEY_HOME:
                        selected_field = 0;
                        break;
                case Q_KEY_END:
                        selected_field = SORT_METHOD_MAX - 1;
                        break;
                case Q_KEY_ENTER:
                case Q_KEY_F(10):
                case C_CR:
                        /* The OK exit point */
                        screen_delwin(pick_window);
                        return selected_field;
                default:
                        /* Ignore */
                        break;
                }
        }

        /* Should never get here. */
        return -1;
} /* ---------------------------------------------------------------------- */

/*
 * Popup the delete entries/notes pick box
 */
static int delete_popup() {
        void * pick_window;
        int window_left;
        int window_top;
        int window_height;
        int window_length;
        char * title;

        int title_left;
        int keystroke;

        /* Status string */
        char * status_string;
        int status_left_stop;

        /* Put up the status line */
        screen_put_color_hline_yx(HEIGHT - 1, 0, cp437_chars[HATCH], WIDTH, Q_COLOR_STATUS);

        status_string = _(" 1-Delete Attached Notes   2-Delete Entries and Notes   ESC/`-Exit ");
        status_left_stop = WIDTH - strlen(status_string);
        if (status_left_stop <= 0) {
                status_left_stop = 0;
        } else {
                status_left_stop /= 2;
        }
        screen_put_color_str_yx(HEIGHT - 1, status_left_stop, status_string, Q_COLOR_STATUS);

        screen_flush();

        window_height = 8;
        window_length = 34;

        /* Window will be centered on the screen */
        window_left = WIDTH - 1 - window_length;
        if (window_left < 0) {
                window_left = 0;
        } else {
                window_left /= 2;
        }
        /* On the bottom 1/3 */
        window_top = (HEIGHT - 1 - window_height) * 2;
        if (window_top < 0) {
                window_top = 0;
        } else {
                window_top /= 3;
        }

        /* Draw the sub-window */
        pick_window = screen_subwin(window_height, window_length, window_top, window_left);
        if (check_subwin_result(pick_window) == Q_FALSE) {
                return -1;
        }

        screen_win_draw_box(pick_window, 0, 0, window_length, window_height);

        /* Place the title */
        title = _("Delete Entries and/or Notes");
        title_left = window_length - (strlen(title) + 2);
        if (title_left < 0) {
                title_left = 0;
        } else {
                title_left /= 2;
        }
        screen_win_put_color_printf_yx(pick_window, 0, title_left, Q_COLOR_WINDOW_BORDER, " %s ", title);

        screen_win_put_color_str_yx(pick_window, 2, 6, "  1.", Q_COLOR_MENU_COMMAND);
        screen_win_put_color_str(pick_window, _(" Notes Only"), Q_COLOR_MENU_TEXT);
        screen_win_put_color_str_yx(pick_window, 3, 6, "  2.", Q_COLOR_MENU_COMMAND);
        screen_win_put_color_str(pick_window, _(" Entries & Notes"), Q_COLOR_MENU_TEXT);
        screen_win_put_color_str_yx(pick_window, 4, 6, _("ESC."), Q_COLOR_MENU_COMMAND);
        screen_win_put_color_str(pick_window, _(" Return to Directory"), Q_COLOR_MENU_TEXT);
        screen_win_put_color_str_yx(pick_window, 6, 2, _("Your Choice ? "), Q_COLOR_MENU_TEXT);
        screen_flush();
        screen_win_flush(pick_window);

        for (;;) {

                qodem_win_getch(pick_window, &keystroke, NULL, Q_KEYBOARD_DELAY);
                switch (keystroke) {
                case '`':
                case KEY_ESCAPE:
                        /* The abort exit point */
                        screen_delwin(pick_window);
                        return -1;
                case '1':
                        /* Notes Only */
                        return 1;
                case '2':
                        /* Entries and Notes */
                        return 2;
                default:
                        /* Ignore */
                        break;
                }
        }

        /* Should never get here. */
        return -1;
} /* ---------------------------------------------------------------------- */

#define DOORWAY_STRING_SIZE 32

/*
 * Popup the doorway pick box
 */
static Q_DOORWAY pick_doorway() {
        void * pick_window;
        int window_left;
        int window_top;
        int window_height;
        int window_length;
        char * title;
        char selection_buffer[DOORWAY_STRING_SIZE];

        int title_left;
        int keystroke;
        Q_DOORWAY i;
        Q_DOORWAY selected_field;

        title = _("Choose Doorway Option");

        window_height = 6;
        window_length = strlen(title) + 4;

        /* Window will be centered on the screen */
        window_left = WIDTH - 1 - window_length;
        if (window_left < 0) {
                window_left = 0;
        } else {
                window_left /= 2;
        }
        window_top = HEIGHT - 1 - window_height;
        if (window_top < 0) {
                window_top = 0;
        } else {
                window_top /= 2;
        }

        /* Draw the sub-window */
        pick_window = screen_subwin(window_height, window_length, window_top, window_left);
        if (check_subwin_result(pick_window) == Q_FALSE) {
                return -1;
        }

        screen_win_draw_box(pick_window, 0, 0, window_length, window_height);

        /* Place the title */
        title_left = window_length - (strlen(title) + 2);
        if (title_left < 0) {
                title_left = 0;
        } else {
                title_left /= 2;
        }
        screen_win_put_color_printf_yx(pick_window, 0, title_left, Q_COLOR_WINDOW_BORDER, " %s ", title);

        selected_field = Q_DOORWAY_CONFIG;

        for (;;) {

                for (i=Q_DOORWAY_CONFIG; i <= Q_DOORWAY_NEVER; i++) {

                        switch (i) {
                        case Q_DOORWAY_CONFIG:
                                snprintf(selection_buffer, sizeof(selection_buffer), " %s", _("Use Global Option"));
                                break;
                        case Q_DOORWAY_ALWAYS_DOORWAY:
                                snprintf(selection_buffer, sizeof(selection_buffer), " %s", _("Always DOORWAY"));
                                break;
                        case Q_DOORWAY_ALWAYS_MIXED:
                                snprintf(selection_buffer, sizeof(selection_buffer), " %s", _("Always MIXED"));
                                break;
                        case Q_DOORWAY_NEVER:
                                snprintf(selection_buffer, sizeof(selection_buffer), " %s", _("Never"));
                                break;
                        }

                        if (strlen(selection_buffer) < window_length - 3) {
                                memset(selection_buffer + strlen(selection_buffer), ' ', window_length - 2 - strlen(selection_buffer));
                                selection_buffer[window_length - 2] = '\0';
                        }

                        if (selected_field == i) {
                                screen_win_put_color_str_yx(pick_window, i + 1, 1, selection_buffer, Q_COLOR_PHONEBOOK_SELECTED);
                        } else {
                                screen_win_put_color_str_yx(pick_window, i + 1, 1, selection_buffer, Q_COLOR_PHONEBOOK_ENTRY);
                        }
                }

                screen_win_flush(pick_window);
                screen_flush();

                qodem_win_getch(pick_window, &keystroke, NULL, Q_KEYBOARD_DELAY);
                switch (keystroke) {
                case '`':
                case KEY_ESCAPE:
                        /* The abort exit point */
                        screen_delwin(pick_window);
                        return -1;
                case Q_KEY_DOWN:
                        selected_field++;
                        if (selected_field > Q_DOORWAY_NEVER) {
                                selected_field = Q_DOORWAY_CONFIG;
                        }
                        break;
                case Q_KEY_UP:
                        selected_field--;
                        if ((selected_field < Q_DOORWAY_CONFIG) || (selected_field > Q_DOORWAY_NEVER)) {
                                selected_field = Q_DOORWAY_NEVER;
                        }
                        break;
                case Q_KEY_HOME:
                        selected_field = Q_DOORWAY_CONFIG;
                        break;
                case Q_KEY_END:
                        selected_field = Q_DOORWAY_NEVER;
                        break;
                case Q_KEY_ENTER:
                case Q_KEY_F(10):
                case C_CR:
                        /* The OK exit point */
                        screen_delwin(pick_window);
                        return selected_field;
                default:
                        /* Ignore */
                        break;
                }
        }

        /* Should never get here. */
        return -1;
} /* ---------------------------------------------------------------------- */

/* Prompt for toggles */
static Q_BOOL toggles_form(const char * title, int * toggles) {
        void * form_window;
        int status_left_stop;
        char * status_string;
        int window_left;
        int window_top;
        int window_height = 19;
        int window_length = 37;
        int title_left;
        int keystroke;
        Q_BOOL local_dirty;

        /* The new version */
        int new_toggles = *toggles;

        /* Window will be 1/3 down the screen */
        window_left = WIDTH - 1 - window_length;
        if (window_left < 0) {
                window_left = 0;
        } else {
                window_left /= 2;
        }
        window_top = (HEIGHT - 1 - window_height);
        if (window_top < 0) {
                window_top = 0;
        } else {
                window_top /= 3;
        }

        /* Put up the status line */
        screen_put_color_hline_yx(HEIGHT - 1, 0, cp437_chars[HATCH], WIDTH, Q_COLOR_STATUS);
        status_string = _(" LETTER-Select a Toggle   ENTER-Done   ESC/`-Exit ");
        status_left_stop = WIDTH - strlen(status_string);
        if (status_left_stop <= 0) {
                status_left_stop = 0;
        } else {
                status_left_stop /= 2;
        }
        screen_put_color_str_yx(HEIGHT - 1, status_left_stop, status_string, Q_COLOR_STATUS);

        /* Draw the sub-window */
        form_window = screen_subwin(window_height, window_length, window_top, window_left);
        if (check_subwin_result(form_window) == Q_FALSE) {
                /* Force a repaint */
                q_screen_dirty = Q_TRUE;
                return Q_FALSE;
        }

        /* Draw box */
        screen_win_draw_box(form_window, 0, 0, window_length, window_height);

        /* Place the title */
        title_left = window_length - (strlen(title) + 2);
        if (title_left < 0) {
                title_left = 0;
        } else {
                title_left /= 2;
        }
        screen_win_put_color_printf_yx(form_window, 0, title_left, Q_COLOR_WINDOW_BORDER, " %s ", title);

        local_dirty = Q_TRUE;

        for (;;) {
                if (local_dirty == Q_TRUE) {
                        /* Re-draw the screen*/

                        screen_win_put_color_str_yx(form_window,  2, 4, "0", Q_COLOR_MENU_COMMAND);
                        if (new_toggles & TOGGLE_SESSION_LOG) {
                                screen_win_put_color_str_yx(form_window,  2, 7, _("Session Log ON "), Q_COLOR_MENU_TEXT);
                        } else {
                                screen_win_put_color_str_yx(form_window,  2, 7, _("Session Log OFF"), Q_COLOR_MENU_TEXT);
                        }

                        screen_win_put_color_str_yx(form_window,  3, 4, "1", Q_COLOR_MENU_COMMAND);
                        if (new_toggles & TOGGLE_XONXOFF) {
                                screen_win_put_color_str_yx(form_window,  3, 7, _("XON/XOFF ON "), Q_COLOR_MENU_TEXT);
                        } else {
                                screen_win_put_color_str_yx(form_window,  3, 7, _("XON/XOFF OFF"), Q_COLOR_MENU_TEXT);
                        }

                        screen_win_put_color_str_yx(form_window,  4, 4, "2", Q_COLOR_MENU_COMMAND);
                        if (new_toggles & TOGGLE_HARD_BACKSPACE) {
                                screen_win_put_color_str_yx(form_window,  4, 7, _("Backspace is ^H "), Q_COLOR_MENU_TEXT);
                        } else {
                                screen_win_put_color_str_yx(form_window,  4, 7, _("Backspace is DEL"), Q_COLOR_MENU_TEXT);
                        }

                        screen_win_put_color_str_yx(form_window,  5, 4, "3", Q_COLOR_MENU_COMMAND);
                        if (new_toggles & TOGGLE_LINEWRAP) {
                                screen_win_put_color_str_yx(form_window,  5, 7, _("Line Wrap OFF"), Q_COLOR_MENU_TEXT);
                        } else {
                                screen_win_put_color_str_yx(form_window,  5, 7, _("Line Wrap ON "), Q_COLOR_MENU_TEXT);
                        }

                        screen_win_put_color_str_yx(form_window,  6, 4, "4", Q_COLOR_MENU_COMMAND);
                        if (new_toggles & TOGGLE_DISPLAY_NULL) {
                                screen_win_put_color_str_yx(form_window,  6, 7, _("Display NULL ON "), Q_COLOR_MENU_TEXT);
                        } else {
                                screen_win_put_color_str_yx(form_window,  6, 7, _("Display NULL OFF"), Q_COLOR_MENU_TEXT);
                        }

                        screen_win_put_color_str_yx(form_window,  7, 4, "7", Q_COLOR_MENU_COMMAND);
                        if (new_toggles & TOGGLE_STATUS_LINE_INFO) {
                                screen_win_put_color_str_yx(form_window,  7, 7, _("Status Line - Info  "), Q_COLOR_MENU_TEXT);
                        } else {
                                screen_win_put_color_str_yx(form_window,  7, 7, _("Status Line - Normal"), Q_COLOR_MENU_TEXT);
                        }

                        screen_win_put_color_str_yx(form_window,  8, 4, "8", Q_COLOR_MENU_COMMAND);
                        if (new_toggles & TOGGLE_STRIP_8TH) {
                                screen_win_put_color_str_yx(form_window,  8, 7, _("Strip 8th ON "), Q_COLOR_MENU_TEXT);
                        } else {
                                screen_win_put_color_str_yx(form_window,  8, 7, _("Strip 8th OFF"), Q_COLOR_MENU_TEXT);
                        }

                        screen_win_put_color_str_yx(form_window,  9, 4, "B", Q_COLOR_MENU_COMMAND);
                        if (new_toggles & TOGGLE_BEEPS) {
                                screen_win_put_color_str_yx(form_window,  9, 7, _("Beeps & Bells OFF"), Q_COLOR_MENU_TEXT);
                        } else {
                                screen_win_put_color_str_yx(form_window,  9, 7, _("Beeps & Bells ON "), Q_COLOR_MENU_TEXT);
                        }

                        screen_win_put_color_str_yx(form_window,  10, 4, "E", Q_COLOR_MENU_COMMAND);
                        if (new_toggles & TOGGLE_HALF_DUPLEX) {
                                screen_win_put_color_str_yx(form_window,  10, 7, _("Half Duplex"), Q_COLOR_MENU_TEXT);
                        } else {
                                screen_win_put_color_str_yx(form_window,  10, 7, _("Full Duplex"), Q_COLOR_MENU_TEXT);
                        }

                        screen_win_put_color_str_yx(form_window,  11, 4, "U", Q_COLOR_MENU_COMMAND);
                        if (new_toggles & TOGGLE_SCROLLBACK) {
                                screen_win_put_color_str_yx(form_window,  11, 7, _("Scrollback OFF"), Q_COLOR_MENU_TEXT);
                        } else {
                                screen_win_put_color_str_yx(form_window,  11, 7, _("Scrollback ON "), Q_COLOR_MENU_TEXT);
                        }

                        screen_win_put_color_str_yx(form_window,  12, 4, "-", Q_COLOR_MENU_COMMAND);
                        if (new_toggles & TOGGLE_STATUS_LINE) {
                                screen_win_put_color_str_yx(form_window,  12, 7, _("Status Line OFF"), Q_COLOR_MENU_TEXT);
                        } else {
                                screen_win_put_color_str_yx(form_window,  12, 7, _("Status Line ON "), Q_COLOR_MENU_TEXT);
                        }

                        screen_win_put_color_str_yx(form_window,  13, 4, "+", Q_COLOR_MENU_COMMAND);
                        if (new_toggles & TOGGLE_CRLF) {
                                screen_win_put_color_str_yx(form_window,  13, 7, _("Add LF ON "), Q_COLOR_MENU_TEXT);
                        } else {
                                screen_win_put_color_str_yx(form_window,  13, 7, _("Add LF OFF"), Q_COLOR_MENU_TEXT);
                        }

                        screen_win_put_color_str_yx(form_window,  14, 4, ",", Q_COLOR_MENU_COMMAND);
                        if (new_toggles & TOGGLE_ANSI_MUSIC) {
                                screen_win_put_color_str_yx(form_window,  14, 7, _("Ansi Music OFF"), Q_COLOR_MENU_TEXT);
                        } else {
                                screen_win_put_color_str_yx(form_window,  14, 7, _("Ansi Music ON "), Q_COLOR_MENU_TEXT);
                        }


                        q_cursor_on();
                        screen_win_put_color_str_yx(form_window, 16, 9, _("Your Choice ? "), Q_COLOR_MENU_COMMAND);

                        screen_flush();
                        screen_win_flush(form_window);

                        local_dirty = Q_FALSE;
                }

                qodem_win_getch(form_window, &keystroke, NULL, Q_KEYBOARD_DELAY);

                if (!q_key_code_yes(keystroke)) {
                        /* Regular character, process and refresh */
                        switch (keystroke) {

                        case '0':
                                new_toggles ^= TOGGLE_SESSION_LOG;
                                break;
                        case '1':
                                new_toggles ^= TOGGLE_XONXOFF;
                                break;
                        case '2':
                                new_toggles ^= TOGGLE_HARD_BACKSPACE;
                                break;
                        case '3':
                                new_toggles ^= TOGGLE_LINEWRAP;
                                break;
                        case '4':
                                new_toggles ^= TOGGLE_DISPLAY_NULL;
                                break;
                        case '7':
                                new_toggles ^= TOGGLE_STATUS_LINE_INFO;
                                break;
                        case '8':
                                new_toggles ^= TOGGLE_STRIP_8TH;
                                break;
                        case 'B':
                        case 'b':
                                new_toggles ^= TOGGLE_BEEPS;
                                break;
                        case 'E':
                        case 'e':
                                new_toggles ^= TOGGLE_HALF_DUPLEX;
                                break;
                        case 'U':
                        case 'u':
                                new_toggles ^= TOGGLE_SCROLLBACK;
                                break;
                        case '-':
                                new_toggles ^= TOGGLE_STATUS_LINE;
                                break;
                        case '+':
                                new_toggles ^= TOGGLE_CRLF;
                                break;
                        case ',':
                                new_toggles ^= TOGGLE_ANSI_MUSIC;
                                break;

                        default:
                                /* Disregard */
                                break;
                        }

                        /* Refresh form window */
                        local_dirty = Q_TRUE;
                }

                switch (keystroke) {

                case '`':
                case KEY_ESCAPE:
                        /* Force a repaint */
                        q_screen_dirty = Q_TRUE;
                        return Q_FALSE;

                case Q_KEY_ENTER:
                case Q_KEY_F(10):
                case C_CR:

                        /* The OK exit point */

                        /* Save new values */
                        *toggles = new_toggles;

                        /* Force a repaint */
                        q_screen_dirty = Q_TRUE;
                        return Q_TRUE;

                default:
                        /* Disregard keystroke */
                        break;
                }
        }

        /* Should never get here. */
        return Q_FALSE;
} /* ---------------------------------------------------------------------- */

/*
 * Edit the linked script
 */
static void spawn_script_editor(const char * script_filename) {
        char command_line[COMMAND_LINE_SIZE];

        if (strlen(script_filename) > 0) {
                screen_clear();
                screen_put_str_yx(0, 0, _("Spawning editor...\n\n"), Q_A_NORMAL, 0);
                screen_flush();
                sprintf(command_line, "%s %s", get_option(Q_OPTION_EDITOR),
                        get_scriptdir_filename(script_filename));
                spawn_terminal(command_line);
#ifndef __BORLANDC__
                if (access(get_scriptdir_filename(script_filename), X_OK) != 0) {
                        /* Script exists, make it executable */
                        chmod(get_scriptdir_filename(script_filename), S_IRUSR | S_IWUSR | S_IXUSR);
                }
#endif
        }
} /* ---------------------------------------------------------------------- */

/*
 * Edit a phone entry.
 */
static void edit_phone_entry_form(struct q_phone_struct * entry) {
#ifdef Q_NO_SERIAL
        struct field * fields[13];
#else
        struct field * fields[14];
#endif /* Q_NO_SERIAL */
        struct fieldset * edit_form;
        struct field * port_field;
        void * form_window;
        int color_active = Q_COLOR_WINDOW_FIELD_TEXT_HIGHLIGHTED;
        int color_inactive = Q_COLOR_WINDOW_FIELD_HIGHLIGHTED;
        int status_left_stop;
        char * status_string = NULL;
        int window_left;
        int window_top;
        int window_height = 20;
        int window_length = 51;
        char * title;
        int title_left;
        int keystroke;
        int new_keystroke;
        int flags;
        int i;
        char * form_value_string;
        /* I know for sure that this string will be short */
        char toggles_string[32];

        struct file_info * file_selection;

        /* Local copies of the fields being edited */
        wchar_t * name;
        char * address;
        char * port;
        wchar_t * username;
        wchar_t * password;
        char * password_stars;
        char * script_filename;
        char * capture_filename;
        char * keybindings_filename;
        Q_DIAL_METHOD method;
        Q_EMULATION emulation;
        Q_CODEPAGE codepage;

#ifndef Q_NO_SERIAL
        Q_BAUD_RATE baud;
        DATA_BITS data_bits;
        STOP_BITS stop_bits;
        Q_PARITY parity;
        Q_BOOL use_modem_cfg;
        Q_BOOL xonxoff;
        Q_BOOL rtscts;
        Q_BOOL lock_dte_baud;

        /* I know for sure that this string will be short */
        char comm_settings_string[32];
#endif /* Q_NO_SERIAL */

        Q_DOORWAY doorway;
        Q_BOOL use_default_toggles;
        int toggles;

        char time_string[TIME_STRING_LENGTH];
        Q_BOOL local_dirty;
        Q_BOOL real_dirty;
        Q_BOOL dont_reload = Q_FALSE;
        Q_BOOL must_use_picklist = Q_FALSE;
        enum fields {
                NAME,
                ADDRESS,
                PORT,
                METHOD,
                USERNAME,
                PASSWORD,
                SCRIPT_NAME,
                EMULATION,
                CODEPAGE,
                CAPTUREFILE_NAME,
                KEYBINDINGS_NAME,
                DOORWAY,
#ifndef Q_NO_SERIAL
                COMM_SETTINGS,
#endif /* Q_NO_SERIAL */
                TOGGLES,

                /* No more form fields */
                CLEAR_CALL_INFO
        };
        int field_number;

        /* Window will be 1/3 down the screen */
        window_left = WIDTH - 1 - window_length;
        if (window_left < 0) {
                window_left = 0;
        } else {
                window_left /= 2;
        }
        window_top = HEIGHT - 1 - window_height;
        if (window_top < 0) {
                window_top = 0;
        } else {
                window_top /= 3;
        }

        /* Draw the sub-window */
        form_window = screen_subwin(window_height, window_length, window_top, window_left);
        if (check_subwin_result(form_window) == Q_FALSE) {
                return;
        }

        /* width, toprow, leftcol, fixed, color_active, color_inactive */
        fields[0] = field_malloc(32,  1, 16, Q_FALSE, color_active, color_inactive);    /* NAME */
        fields[1] = field_malloc(32,  2, 16, Q_FALSE, color_active, color_inactive);    /* ADDRESS */
        fields[2] = field_malloc( 5,  3, 16, Q_FALSE, color_active, color_inactive);    /* PORT */
        port_field = fields[2];
        fields[3] = field_malloc( 7,  4, 16, Q_TRUE, color_active, color_inactive);     /* METHOD */
        fields[4] = field_malloc(32,  5, 16, Q_FALSE, color_active, color_inactive);    /* USERNAME */
        fields[5] = field_malloc(32,  6, 16, Q_FALSE, color_active, color_inactive);    /* PASSWORD */
        fields[6] = field_malloc(32,  7, 16, Q_FALSE, color_active, color_inactive);    /* SCRIPT_NAME */
        fields[7] = field_malloc( 6,  8, 16, Q_TRUE, color_active, color_inactive);     /* EMULATION */
        fields[8] = field_malloc(15,  9, 16, Q_TRUE, color_active, color_inactive);     /* CODEPAGE */
        fields[9] = field_malloc(32,  10, 16, Q_FALSE, color_active, color_inactive);   /* CAPTUREFILE_NAME */
        fields[10] = field_malloc(32, 11, 16, Q_FALSE, color_active, color_inactive);   /* KEYBINDINGS_NAME */
        fields[11] = field_malloc(32, 12, 16, Q_TRUE, color_active, color_inactive);    /* DOORWAY */
#ifdef Q_NO_SERIAL
        fields[12] = field_malloc(32, 14, 16, Q_TRUE, color_active, color_inactive);    /* TOGGLES */
        edit_form = fieldset_malloc(fields, 13, form_window);
#else
        fields[12] = field_malloc(32, 13, 16, Q_TRUE, color_active, color_inactive);    /* COMM_SETTINGS */
        fields[13] = field_malloc(32, 14, 16, Q_TRUE, color_active, color_inactive);    /* TOGGLES */
        edit_form = fieldset_malloc(fields, 14, form_window);
#endif /* Q_NO_SERIAL */

        field_number = NAME;

        name                    = Xwcsdup(entry->name, __FILE__, __LINE__);
        address                 = Xstrdup(entry->address, __FILE__, __LINE__);
        port                    = Xstrdup(entry->port, __FILE__, __LINE__);
        method                  = entry->method;
        username                = Xwcsdup(entry->username, __FILE__, __LINE__);
        password                = Xwcsdup(entry->password, __FILE__, __LINE__);
        script_filename         = Xstrdup(entry->script_filename, __FILE__, __LINE__);
        emulation               = entry->emulation;
        codepage                = entry->codepage;
        capture_filename        = Xstrdup(entry->capture_filename, __FILE__, __LINE__);
        keybindings_filename    = Xstrdup(entry->keybindings_filename, __FILE__, __LINE__);

#ifndef Q_NO_SERIAL
        baud                    = entry->baud;
        data_bits               = entry->data_bits;
        parity                  = entry->parity;
        stop_bits               = entry->stop_bits;
        xonxoff                 = entry->xonxoff;
        rtscts                  = entry->rtscts;
        use_modem_cfg           = entry->use_modem_cfg;
        lock_dte_baud           = entry->lock_dte_baud;
#endif /* Q_NO_SERIAL */

        doorway                 = entry->doorway;
        toggles                 = entry->toggles;
        use_default_toggles     = entry->use_default_toggles;

        /* Convert password to stars */
        password_stars = (char *)Xmalloc(wcslen(entry->password) + 1, __FILE__, __LINE__);
        memset(password_stars, 0, wcslen(entry->password) + 1);
        for (i = 0; i < wcslen(entry->password); i++) {
                password_stars[i] = '*';
        }

        real_dirty = Q_TRUE;
        local_dirty = Q_TRUE;
        for (;;) {

                if (local_dirty == Q_TRUE) {

                        if (real_dirty == Q_TRUE) {
                                /* Refresh background */
                                q_screen_dirty = Q_TRUE;
                                phonebook_refresh();

                                /* Draw the box */
                                screen_win_draw_box(form_window, 0, 0, window_length, window_height);

                                /* Place the "F1 Help" text */
                                screen_win_put_color_str_yx(form_window, window_height - 1, window_length - 10, _("F1 Help"), Q_COLOR_WINDOW_BORDER);

                                /* Place the title */
                                title = _("Revise Entry");
                                title_left = window_length - (strlen(title) + 2);
                                if (title_left < 0) {
                                        title_left = 0;
                                } else {
                                        title_left /= 2;
                                }
                                screen_win_put_color_printf_yx(form_window, 0, title_left, Q_COLOR_WINDOW_BORDER, " %s ", title);

                                screen_win_put_color_str_yx(form_window, 1, 2, _("Name"), Q_COLOR_MENU_COMMAND);
                                if (method == Q_DIAL_METHOD_COMMANDLINE) {
                                        screen_win_put_color_str_yx(form_window, 2, 2, _("Command Line"), Q_COLOR_MENU_COMMAND);
#ifndef Q_NO_SERIAL
                                } else if (method == Q_DIAL_METHOD_MODEM) {
                                        screen_win_put_color_str_yx(form_window, 2, 2, _("Phone #"), Q_COLOR_MENU_COMMAND);
#endif /* Q_NO_SERIAL */
                                } else {
                                        screen_win_put_color_str_yx(form_window, 2, 2, _("Address"), Q_COLOR_MENU_COMMAND);
                                }
                                if (    (method == Q_DIAL_METHOD_TELNET) ||
                                        (method == Q_DIAL_METHOD_SSH) ||
                                        (method == Q_DIAL_METHOD_SOCKET)
                                ) {
                                        screen_win_put_color_str_yx(form_window, 3, 2, _("Port"), Q_COLOR_MENU_COMMAND);
                                        port_field->invisible = Q_FALSE;
                                } else {
                                        port_field->invisible = Q_TRUE;
                                }
                                screen_win_put_color_str_yx(form_window, 4, 2, _("Method"), Q_COLOR_MENU_COMMAND);
                                screen_win_put_color_str_yx(form_window, 5, 2, _("Username"), Q_COLOR_MENU_COMMAND);
                                screen_win_put_color_str_yx(form_window, 6, 2, _("Password"), Q_COLOR_MENU_COMMAND);
                                screen_win_put_color_str_yx(form_window, 7, 2, _("Script"), Q_COLOR_MENU_COMMAND);
                                screen_win_put_color_str_yx(form_window, 8, 2, _("Emulation"), Q_COLOR_MENU_COMMAND);
                                screen_win_put_color_str_yx(form_window, 9, 2, _("Codepage"), Q_COLOR_MENU_COMMAND);
                                screen_win_put_color_str_yx(form_window, 10, 2, _("Capture File"), Q_COLOR_MENU_COMMAND);
                                screen_win_put_color_str_yx(form_window, 11, 2, _("Key File"), Q_COLOR_MENU_COMMAND);
                                screen_win_put_color_str_yx(form_window, 12, 2, _("Doorway"), Q_COLOR_MENU_COMMAND);
#ifndef Q_NO_SERIAL
                                screen_win_put_color_str_yx(form_window, 13, 2, _("Port Settings"), Q_COLOR_MENU_COMMAND);
#endif /* Q_NO_SERIAL */
                                screen_win_put_color_str_yx(form_window, 14, 2, _("Toggles"), Q_COLOR_MENU_COMMAND);

                                screen_win_put_color_str_yx(form_window, 17, 2, _("Last Call"), Q_COLOR_MENU_COMMAND);
                                if (entry->times_on > 0) {
                                        strftime(time_string, sizeof(time_string), "%a, %d %b %Y %H:%M:%S", localtime(&entry->last_call));
                                        screen_win_put_color_printf(form_window, Q_COLOR_MENU_TEXT, "    %s", time_string);
                                }
                                screen_win_put_color_str_yx(form_window, 18, 2, _("Times On"), Q_COLOR_MENU_COMMAND);
                                screen_win_put_color_printf(form_window, Q_COLOR_MENU_TEXT, "     %u", entry->times_on);

                                if (dont_reload == Q_FALSE) {
                                        field_set_value(fields[0], name);
                                        field_set_char_value(fields[1], address);
                                        field_set_char_value(fields[2], port);
                                        field_set_char_value(fields[3], method_string(method));
                                        field_set_value(fields[4], username);
                                        if (field_number == PASSWORD) {
                                                /* Replace with the real password */
                                                field_set_value(fields[5], password);
                                        } else {
                                                /* Show the password as stars */
                                                field_set_char_value(fields[5], password_stars);
                                        }
                                        field_set_char_value(fields[6], script_filename);
                                        field_set_char_value(fields[7], emulation_string(emulation));
                                        field_set_char_value(fields[8], codepage_string(codepage));
                                        field_set_char_value(fields[9], capture_filename);
                                        field_set_char_value(fields[10], keybindings_filename);
                                        field_set_char_value(fields[11], doorway_string(doorway));

#ifndef Q_NO_SERIAL
                                        /* Comm settings string */
                                        if (use_modem_cfg == Q_TRUE) {
                                                sprintf(comm_settings_string, _("Use Modem Config"));
                                        } else {
                                                sprintf(comm_settings_string, "%s %s%s%s%s%s%s", baud_string(baud),
                                                        data_bits_string(data_bits), parity_string(parity, Q_TRUE),
                                                        stop_bits_string(stop_bits),
                                                        (xonxoff == Q_TRUE ? " XON/XOFF" : ""),
                                                        (rtscts == Q_TRUE ? " RTS/CTS" : ""),
                                                        (lock_dte_baud == Q_TRUE ? _(" DTE Locked") : "")
                                                );
                                        }
                                        field_set_char_value(fields[12], comm_settings_string);
#endif /* Q_NO_SERIAL */


                                        /* Toggles string */
                                        if (use_default_toggles == Q_TRUE) {
                                                sprintf(toggles_string, _("Default Toggles"));
                                        } else {
                                                sprintf(toggles_string, "%s", toggles_to_string(toggles));
                                        }
#ifdef Q_NO_SERIAL
                                        field_set_char_value(fields[12], toggles_string);
#else
                                        field_set_char_value(fields[13], toggles_string);
#endif /* Q_NO_SERIAL */

                                } /* if (dont_reload == Q_FALSE) */

                                real_dirty = Q_FALSE;
                        } /* if (real_dirty == Q_TRUE) */

                        /* Put up the status line */
                        screen_put_color_hline_yx(HEIGHT - 1, 0, cp437_chars[HATCH], WIDTH, Q_COLOR_STATUS);

                        switch (field_number) {
                        case NAME:
                                status_string = _(""
" Change NAME field                         [F10/Alt-Enter] Save   [ESC] "
"Abort ");
                                break;
                        case ADDRESS:
                                status_string = _(""
" Change ADDRESS/COMMAND/NUMBER Field       [F10/Alt-Enter] Save   [ESC] "
"Abort ");
                                break;
                        case PORT:
                                status_string = _(""
" Change PORT Field                         [F10/Alt-Enter] Save   [ESC] "
"Abort ");
                                break;
                        case METHOD:
                                status_string = _(""
" Change Con. Method      [F2/Space] Pick   [F10/Alt-Enter] Save   [ESC] "
"Abort ");
                                break;
                        case USERNAME:
                                status_string = _(""
" Change USERNAME Field                     [F10/Alt-Enter] Save   [ESC] "
"Abort ");
                                break;
                        case PASSWORD:
                                status_string = _(""
" Change PASSWORD Field                     [F10/Alt-Enter] Save   [ESC] "
"Abort ");
                                break;
                        case SCRIPT_NAME:
                                status_string = _(""
" Change Linked Script          [F2] Edit   [F10/Alt-Enter] Save   [ESC] "
"Abort ");
                                break;
                        case EMULATION:
                                status_string = _(""
" Change Emulation        [F2/Space] Pick   [F10/Alt-Enter] Save   [ESC] "
"Abort ");
                                break;
                        case CODEPAGE:
                                status_string = _(""
" Change Codepage         [F2/Space] Pick   [F10/Alt-Enter] Save   [ESC] "
"Abort ");
                                break;
                        case CAPTUREFILE_NAME:
                                status_string = _(""
" Change Capture File     [F2/Space] Pick   [F10/Alt-Enter] Save   [ESC] "
"Abort ");
                                break;
                        case KEYBINDINGS_NAME:
                                status_string = _(""
" Change Key File               [F2] Edit   [F10/Alt-Enter] Save   [ESC] "
"Abort ");
                                break;
                        case DOORWAY:
                                status_string = _(""
" Change Doorway Option   [F2/Space] Edit   [F10/Alt-Enter] Save   [ESC] "
"Abort ");
                                break;
#ifndef Q_NO_SERIAL
                        case COMM_SETTINGS:
                                status_string = _(""
" Change Port Settings    [F2/Space] Edit   [F10/Alt-Enter] Save   [ESC] "
"Abort ");
                                break;
#endif /* Q_NO_SERIAL */
                        case TOGGLES:
                                status_string = _(""
" Change Toggles          [F2/Space] Edit   [F10/Alt-Enter] Save   [ESC] "
"Abort ");
                                break;
                        case CLEAR_CALL_INFO:
                                status_string = _(""
" Clear Call Information    [ENTER] Clear   [F10/Alt-Enter] Save   [ESC] "
"Abort ");
                                break;
                        }

                        local_dirty = Q_FALSE;

                        status_left_stop = WIDTH - strlen(status_string);
                        if (status_left_stop <= 0) {
                                status_left_stop = 0;
                        } else {
                                status_left_stop /= 2;
                        }
                        screen_put_color_str_yx(HEIGHT - 1, status_left_stop, status_string, Q_COLOR_STATUS);

                        if (field_number == CLEAR_CALL_INFO) {
                                screen_win_put_color_str_yx(form_window, 15, 1, _(" Clear Call Info "), Q_COLOR_PHONEBOOK_SELECTED_TAGGED);
                        } else {
                                screen_win_put_color_str_yx(form_window, 15, 1, _(" Clear Call Info "), Q_COLOR_MENU_COMMAND);
                                if (dont_reload == Q_FALSE) {
                                        fieldset_home_char(edit_form);
                                }
                        }

                        screen_win_flush(form_window);
                        screen_flush();
                        fieldset_render(edit_form);

                        dont_reload = Q_FALSE;
                }  /* if (local_dirty == Q_TRUE) */

                /*
                 * Some fields I don't want the cursor on or the ability to
                 * directly edit.
                 */
                must_use_picklist = Q_FALSE;
                if (    (field_number == METHOD) ||
                        (field_number == EMULATION) ||
                        (field_number == CODEPAGE) ||
                        (field_number == DOORWAY) ||
#ifndef Q_NO_SERIAL
                        (field_number == COMM_SETTINGS) ||
#endif /* Q_NO_SERIAL */
                        (field_number == TOGGLES)
                ) {
                        must_use_picklist = Q_TRUE;
                }

                qodem_win_getch(form_window, &keystroke, &flags, Q_KEYBOARD_DELAY);

                /* Support alternate keys */
                if ((keystroke == Q_KEY_ENTER) ||
                        (keystroke == 0x0A) ||
                        (keystroke == 0x0D)
                ) {
                        if (flags & KEY_FLAG_ALT) {
                                keystroke = Q_KEY_F(10);
                        }
                }

                switch (keystroke) {

                case ERR:
                        /* Do nothing */
                        break;

                case Q_KEY_F(1):
                        launch_help(Q_HELP_PHONEBOOK_REVISE_ENTRY);

                        /* Refresh the whole screen */
                        local_dirty = Q_TRUE;
                        real_dirty = Q_TRUE;
                        break;

                case KEY_ESCAPE:
                        /* The abort exit point */
                        fieldset_free(edit_form);
                        screen_delwin(form_window);
                        Xfree(password_stars, __FILE__, __LINE__);

                        /* No leak */
                        if (name != NULL) {
                                Xfree(name, __FILE__, __LINE__);
                        }
                        if (address != NULL) {
                                Xfree(address, __FILE__, __LINE__);
                        }
                        if (port != NULL) {
                                Xfree(port, __FILE__, __LINE__);
                        }
                        if (password != NULL) {
                                Xfree(password, __FILE__, __LINE__);
                        }
                        if (username != NULL) {
                                Xfree(username, __FILE__, __LINE__);
                        }
                        if (script_filename != NULL) {
                                Xfree(script_filename, __FILE__, __LINE__);
                        }
                        if (capture_filename != NULL) {
                                Xfree(capture_filename, __FILE__, __LINE__);
                        }
                        if (keybindings_filename != NULL) {
                                Xfree(keybindings_filename, __FILE__, __LINE__);
                        }
                        return;

                case Q_KEY_ENTER:
                case C_CR:
                        if (field_number == CLEAR_CALL_INFO) {
                                entry->times_on = 0;
                                entry->last_call = 0;

                                /* Save settings ---------------------------- */

                                if (name != NULL) {
                                        Xfree(name, __FILE__, __LINE__);
                                }
                                name                            = field_get_value(fields[0]);
                                if (address != NULL) {
                                        Xfree(address, __FILE__, __LINE__);
                                }
                                address                         = field_get_char_value(fields[1]);
                                if (port != NULL) {
                                        Xfree(port, __FILE__, __LINE__);
                                }
                                port                            = field_get_char_value(fields[2]);
                                form_value_string = field_get_char_value(fields[3]);
                                method                          = method_from_string(form_value_string);
                                Xfree(form_value_string, __FILE__, __LINE);
                                if (username != NULL) {
                                        Xfree(username, __FILE__, __LINE__);
                                }
                                username                                = field_get_value(fields[4]);

                                if (script_filename != NULL) {
                                        Xfree(script_filename, __FILE__, __LINE__);
                                }
                                script_filename                 = field_get_char_value(fields[6]);
                                form_value_string = field_get_char_value(fields[7]);
                                emulation                       = emulation_from_string(form_value_string);
                                Xfree(form_value_string, __FILE__, __LINE);
                                form_value_string = field_get_char_value(fields[8]);
                                codepage                        = codepage_from_string(form_value_string);
                                Xfree(form_value_string, __FILE__, __LINE);
                                if (capture_filename != NULL) {
                                        Xfree(capture_filename, __FILE__, __LINE__);
                                }
                                capture_filename                = field_get_char_value(fields[9]);
                                if (keybindings_filename != NULL) {
                                        Xfree(keybindings_filename, __FILE__, __LINE__);
                                }
                                keybindings_filename            = field_get_char_value(fields[10]);
                                doorway                         = doorway_from_string(field_get_char_value(fields[11]));

                                /* Save settings ---------------------------- */

                                local_dirty = Q_TRUE;
                                real_dirty = Q_TRUE;
                                break;
                        }

                        /* Fall through ... */
                case Q_KEY_DOWN:
                        if (field_number < CLEAR_CALL_INFO) {
                                if (field_number < TOGGLES) {
                                        fieldset_next_field(edit_form);
                                        edit_form->inactive = Q_FALSE;
                                } else {
                                        edit_form->inactive = Q_TRUE;
                                }


                                if (field_number == PASSWORD) {
                                        /* Leaving the password field... */
                                        /* Save the edited password */

                                        /* No leak */
                                        if (password != NULL) {
                                                Xfree(password, __FILE__, __LINE__);
                                        }

                                        password = field_get_value(fields[5]);
                                        /* Rebuild password_stars */
                                        Xfree(password_stars, __FILE__, __LINE);
                                        password_stars = (char *)Xmalloc(wcslen(password) + 1, __FILE__, __LINE__);
                                        memset(password_stars, 0, wcslen(password) + 1);
                                        for (i = 0; i < wcslen(password); i++) {
                                                password_stars[i] = '*';
                                        }

                                        /* Replace with the password stars */
                                        field_set_char_value(fields[5], password_stars);
                                } else if (field_number == ADDRESS) {
                                        /* Leaving address */
                                        form_value_string = field_get_char_value(fields[3]);
                                        if (    (method_from_string(form_value_string) != Q_DIAL_METHOD_TELNET) &&
                                                (method_from_string(form_value_string) != Q_DIAL_METHOD_SSH) &&
                                                (method_from_string(form_value_string) != Q_DIAL_METHOD_SOCKET)
                                        ) {
                                                /* Skip over port */
                                                field_number++;
                                                fieldset_next_field(edit_form);
                                        }
                                        Xfree(form_value_string, __FILE__, __LINE);
                                }

                                if ((must_use_picklist == Q_TRUE) && (field_number != TOGGLES)) {
                                        /* Leaving the picklist fields */
                                        q_cursor_on();
                                }

                                field_number++;
                                local_dirty = Q_TRUE;
                                if (field_number == CLEAR_CALL_INFO) {
                                        /*
                                         * I guess I have to call field_form_driver to force it
                                         * to refresh it's display.
                                         */
                                        fieldset_render(edit_form);
                                }

                                if (field_number == PASSWORD) {
                                        /* Entering the password field... */
                                        /* Replace with the real password */
                                        field_set_value(fields[5], password);
                                }

                                /* Not pretty, I just duplicated the must_use_picklist check */
                                if (    (field_number == METHOD) ||
                                        (field_number == EMULATION) ||
                                        (field_number == CODEPAGE) ||
                                        (field_number == DOORWAY) ||
#ifndef Q_NO_SERIAL
                                        (field_number == COMM_SETTINGS) ||
#endif /* Q_NO_SERIAL */
                                        (field_number == TOGGLES)
                                ) {
                                        q_cursor_off();
                                }
                        }
                        break;

                case Q_KEY_UP:
                        if (field_number > NAME) {
                                if (field_number < CLEAR_CALL_INFO) {
                                        fieldset_prev_field(edit_form);
                                }
                                edit_form->inactive = Q_FALSE;

                                if (field_number == PASSWORD) {
                                        /* Leaving the password field... */
                                        /* Save the edited password */
                                        /* No leak */
                                        if (password != NULL) {
                                                Xfree(password, __FILE__, __LINE__);
                                        }
                                        password = field_get_value(fields[5]);
                                        /* Rebuild password_stars */
                                        Xfree(password_stars, __FILE__, __LINE);
                                        password_stars = (char *)Xmalloc(wcslen(password) + 1, __FILE__, __LINE__);
                                        memset(password_stars, 0, wcslen(password) + 1);
                                        for (i = 0; i < wcslen(password); i++) {
                                                password_stars[i] = '*';
                                        }

                                        /* Replace with the password stars */
                                        field_set_char_value(fields[5], password_stars);
                                } else if (field_number == METHOD) {
                                        /* Leaving method */
                                        form_value_string = field_get_char_value(fields[3]);
                                        if (    (method_from_string(form_value_string) != Q_DIAL_METHOD_TELNET) &&
                                                (method_from_string(form_value_string) != Q_DIAL_METHOD_SSH) &&
                                                (method_from_string(form_value_string) != Q_DIAL_METHOD_SOCKET)
                                        ) {
                                                /* Skip over port */
                                                field_number--;
                                                fieldset_prev_field(edit_form);
                                        }
                                        Xfree(form_value_string, __FILE__, __LINE);
                                }

                                if ((must_use_picklist == Q_TRUE) || (field_number == CLEAR_CALL_INFO)) {
                                        /* Leaving the picklist fields */
                                        q_cursor_on();
                                }

                                field_number--;
                                local_dirty = Q_TRUE;

                                if (field_number == PASSWORD) {
                                        /* Entering the password field... */
                                        /* Replace with the real password */
                                        field_set_value(fields[5], password);
                                }

                                /* Not pretty, I just duplicated the
                                 * must_use_picklist check */
                                if (    (field_number == METHOD) ||
                                        (field_number == EMULATION) ||
                                        (field_number == CODEPAGE) ||
                                        (field_number == DOORWAY) ||
#ifndef Q_NO_SERIAL
                                        (field_number == COMM_SETTINGS) ||
#endif /* Q_NO_SERIAL */
                                        (field_number == TOGGLES)
                                ) {
                                        q_cursor_off();
                                }
                        }

                        break;

                case ' ':
                        /*
                         * This is deliberately copied verbatim from the switch
                         * default case so that it can fall through into the
                         * F2 case.  This way, Space and F2 do the same thing.
                         *
                         * If the default case changes, this might need to also.
                         */
                        if (!q_key_code_yes(keystroke)) {
                                if (must_use_picklist == Q_FALSE) {
                                        /* Pass normal keys to form driver */
                                        fieldset_keystroke(edit_form, keystroke);
                                        break;
                                }
                        }
                        /* Fall through... */

                case Q_KEY_F(2):
                        if (must_use_picklist == Q_TRUE) {
                                /* Put up a picklist */
                                switch (field_number) {
                                case METHOD:
                                        method = pick_method();
                                        if (method != -1) {
                                                field_set_char_value(fields[3], method_string(method));
                                                if (port != NULL) {
                                                        Xfree(port, __FILE__, __LINE__);
                                                        port = NULL;
                                                }
                                                port = default_port(method);
                                                field_set_char_value(fields[2], port);
                                        } else {
                                                method = entry->method;
                                        }

                                        if (    (method == Q_DIAL_METHOD_TELNET) ||
                                                (method == Q_DIAL_METHOD_SSH) ||
                                                (method == Q_DIAL_METHOD_SOCKET)
                                        ) {
                                                port_field->invisible = Q_FALSE;
                                        } else {
                                                port_field->invisible = Q_TRUE;
                                        }
                                        break;

                                case EMULATION:
                                        emulation = pick_emulation();
                                        if (emulation != -1) {
                                                field_set_char_value(fields[7], emulation_string(emulation));
                                                codepage = default_codepage(emulation);
                                                field_set_char_value(fields[8], codepage_string(codepage));
                                        } else {
                                                emulation = entry->emulation;
                                        }
                                        break;

                                case CODEPAGE:
                                        codepage = pick_codepage(emulation);
                                        if (codepage != -1) {
                                                field_set_char_value(fields[8], codepage_string(codepage));
                                        } else {
                                                codepage = entry->codepage;
                                        }
                                        break;

                                case DOORWAY:
                                        doorway = pick_doorway();
                                        if (doorway != -1) {
                                                field_set_char_value(fields[11], doorway_string(doorway));
                                        } else {
                                                doorway = entry->doorway;
                                        }
                                        break;

#ifndef Q_NO_SERIAL
                                case COMM_SETTINGS:
                                        /*
                                         * Ask the user if they want
                                         * to deviate from the default
                                         * modem configuration.
                                         */
                                        keystroke = tolower(notify_prompt_form(
                                                _("Change Port Settings"),
                                                _("Use Modem Config? [Y/n] "),
                                                _(" Y-Use the Modem Settings   N-Override the Modem Settings for This Entry "),
                                                Q_TRUE,
                                                0.0, "YyNn\r"));

                                        if ((keystroke == 'y') || (keystroke == C_CR)) {
                                                use_modem_cfg = Q_TRUE;
                                        } else {
                                                use_modem_cfg = Q_FALSE;
                                                comm_settings_form(_("Change Port Settings"), &baud, &data_bits, &parity, &stop_bits, &xonxoff, &rtscts);

                                                keystroke = tolower(notify_prompt_form(
                                                        _("DTE Baud"),
                                                        _("Lock DTE Baud? [Y/n] "),
                                                        _(" Y-Lock Serial Port Speed   N-Change Serial Port Speed After CONNECT "),
                                                        Q_TRUE,
                                                        0.0, "YyNn\r"));
                                                if (    (keystroke == 'y') ||
                                                        (keystroke == C_CR)
                                                ) {
                                                        lock_dte_baud = Q_TRUE;
                                                } else {
                                                        lock_dte_baud = Q_FALSE;
                                                }
                                                q_cursor_off();
                                        }
                                        break;
#endif /* Q_NO_SERIAL */

                                case TOGGLES:
                                        /*
                                         * Ask the user if they want
                                         * to deviate from the default
                                         * toggles configuration.
                                         */
                                        keystroke = tolower(notify_prompt_form(
                                                                _("Change Toggles"),
                                                                _("Use Defaults? [Y/n] "),
                                                                _(" Y-Use the Default Settings   N-Override the Toggles for This Entry "),
                                                                Q_TRUE,
                                                                0.0, "YyNn\r"));

                                        if ((keystroke == 'y') || (keystroke == C_CR)) {
                                                use_default_toggles = Q_TRUE;
                                                toggles = 0;
                                        } else {
                                                use_default_toggles = Q_FALSE;
                                                toggles_form(_("Change Toggles"), &toggles);
                                                q_cursor_off();
                                        }
                                        break;

                                default:
                                        break;
                                }

                                local_dirty = Q_TRUE;
                                real_dirty = Q_TRUE;
                        } else {

                                switch (field_number) {

                                case SCRIPT_NAME:
                                        /* Pull up external editor */
                                        if (script_filename != NULL) {
                                                Xfree(script_filename, __FILE__, __LINE__);
                                        }
                                        script_filename                 = field_get_char_value(fields[6]);

                                        spawn_script_editor(script_filename);
                                        local_dirty = Q_TRUE;
                                        real_dirty = Q_TRUE;
                                        break;

                                case CAPTUREFILE_NAME:
                                        /* Pull up the pick list */
                                        file_selection = view_directory(get_option(Q_OPTION_WORKING_DIR), "");

                                        if (file_selection != NULL) {
                                                field_set_char_value(fields[9], basename(file_selection->name));
                                                Xfree(file_selection, __FILE__, __LINE__);
                                        }

                                        /* Refresh the whole screen */
                                        local_dirty = Q_TRUE;
                                        real_dirty = Q_TRUE;
                                        break;

                                case KEYBINDINGS_NAME:
                                        /* Pull up keyboard editor window */
                                        if (keybindings_filename != NULL) {
                                                Xfree(keybindings_filename, __FILE__, __LINE__);
                                        }
                                        keybindings_filename            = field_get_char_value(fields[10]);
                                        if (strlen(keybindings_filename) > 0) {
                                                switch_current_keyboard(keybindings_filename);
                                                switch_state(Q_STATE_FUNCTION_KEY_EDITOR);
                                                /* This is ugly, but necessary. */
                                                while (q_program_state == Q_STATE_FUNCTION_KEY_EDITOR) {
                                                        refresh_handler();
                                                        keyboard_handler();
                                                }
                                                /*
                                                 * switch_state() made some
                                                 * changes that don't work
                                                 * for us
                                                 */
                                                q_cursor_on();

                                                /* Refresh the whole screen */
                                                local_dirty = Q_TRUE;
                                                real_dirty = Q_TRUE;
                                        }
                                        break;

                                default:
                                        break;
                                }

                        } /* if (must_use_picklist == Q_FALSE) */

                        /* Save settings ----------------------------------- */

                        if (name != NULL) {
                                Xfree(name, __FILE__, __LINE__);
                        }
                        name                            = field_get_value(fields[0]);
                        if (address != NULL) {
                                Xfree(address, __FILE__, __LINE__);
                        }
                        address                         = field_get_char_value(fields[1]);
                        if (port != NULL) {
                                Xfree(port, __FILE__, __LINE__);
                        }
                        port                            = field_get_char_value(fields[2]);
                        form_value_string = field_get_char_value(fields[3]);
                        method                          = method_from_string(form_value_string);
                        Xfree(form_value_string, __FILE__, __LINE);
                        if (username != NULL) {
                                Xfree(username, __FILE__, __LINE__);
                        }
                        username                                = field_get_value(fields[4]);

                        if (script_filename != NULL) {
                                Xfree(script_filename, __FILE__, __LINE__);
                        }
                        script_filename                 = field_get_char_value(fields[6]);
                        form_value_string = field_get_char_value(fields[7]);
                        emulation                       = emulation_from_string(form_value_string);
                        Xfree(form_value_string, __FILE__, __LINE);
                        form_value_string = field_get_char_value(fields[8]);
                        codepage                        = codepage_from_string(form_value_string);
                        Xfree(form_value_string, __FILE__, __LINE);
                        if (capture_filename != NULL) {
                                Xfree(capture_filename, __FILE__, __LINE__);
                        }
                        capture_filename                = field_get_char_value(fields[9]);
                        if (keybindings_filename != NULL) {
                                Xfree(keybindings_filename, __FILE__, __LINE__);
                        }
                        keybindings_filename            = field_get_char_value(fields[10]);
                        doorway                         = doorway_from_string(field_get_char_value(fields[11]));

                        /* Save settings ----------------------------------- */

                        break;

                case Q_KEY_BACKSPACE:
                case 0x08:
                        if (must_use_picklist == Q_FALSE) {
                                fieldset_backspace(edit_form);
                        }
                        break;
                case Q_KEY_LEFT:
                        if (must_use_picklist == Q_FALSE) {
                                fieldset_left(edit_form);
                        }
                        break;
                case Q_KEY_RIGHT:
                        if (must_use_picklist == Q_FALSE) {
                                fieldset_right(edit_form);
                        }
                        break;
                case Q_KEY_HOME:
                        if (must_use_picklist == Q_FALSE) {
                                fieldset_home_char(edit_form);
                        }
                        break;
                case Q_KEY_END:
                        if (must_use_picklist == Q_FALSE) {
                                fieldset_end_char(edit_form);
                        }
                        break;
                case Q_KEY_DC:
                        if (must_use_picklist == Q_FALSE) {
                                fieldset_delete_char(edit_form);
                        }
                        break;
                case Q_KEY_IC:
                        if (must_use_picklist == Q_FALSE) {
                                fieldset_insert_char(edit_form);
                        }
                        break;

                case Q_KEY_F(10):
                        /* The OK exit point */



                        /* Save settings ----------------------------------- */

                        if (entry->name != NULL) {
                                Xfree(entry->name, __FILE__, __LINE__);
                        }
                        entry->name                     = field_get_value(fields[0]);
                        if (entry->address != NULL) {
                                Xfree(entry->address, __FILE__, __LINE__);
                        }
                        entry->address                  = field_get_char_value(fields[1]);
                        if (entry->port != NULL) {
                                Xfree(entry->port, __FILE__, __LINE__);
                        }
                        entry->port                     = field_get_char_value(fields[2]);
                        form_value_string = field_get_char_value(fields[3]);
                        entry->method                   = method_from_string(form_value_string);
                        Xfree(form_value_string, __FILE__, __LINE);
                        if (entry->username != NULL) {
                                Xfree(entry->username, __FILE__, __LINE__);
                        }
                        entry->username                 = field_get_value(fields[4]);

                        if (field_number == PASSWORD) {
                                /* We're exiting on the password field */
                                /* Save the edited password */
                                if (password != NULL) {
                                        Xfree(password, __FILE__, __LINE__);
                                }
                                password = field_get_value(fields[5]);
                        }
                        if (entry->password != NULL) {
                                Xfree(entry->password, __FILE__, __LINE__);
                        }
                        entry->password                 = Xwcsdup(password, __FILE__, __LINE__);

                        if (entry->script_filename != NULL) {
                                Xfree(entry->script_filename, __FILE__, __LINE__);
                        }
                        entry->script_filename          = field_get_char_value(fields[6]);
                        form_value_string = field_get_char_value(fields[7]);
                        entry->emulation                = emulation_from_string(form_value_string);
                        Xfree(form_value_string, __FILE__, __LINE);
                        form_value_string = field_get_char_value(fields[8]);
                        entry->codepage         = codepage_from_string(form_value_string);
                        Xfree(form_value_string, __FILE__, __LINE);
                        if (entry->capture_filename != NULL) {
                                Xfree(entry->capture_filename, __FILE__, __LINE__);
                        }
                        entry->capture_filename = field_get_char_value(fields[9]);
                        if (entry->keybindings_filename != NULL) {
                                Xfree(entry->keybindings_filename, __FILE__, __LINE__);
                        }
                        entry->keybindings_filename     = field_get_char_value(fields[10]);
                        entry->doorway                  = doorway_from_string(field_get_char_value(fields[11]));

#ifndef Q_NO_SERIAL
                        entry->use_modem_cfg            = use_modem_cfg;
                        entry->baud                     = baud;
                        entry->data_bits                = data_bits;
                        entry->parity                   = parity;
                        entry->stop_bits                = stop_bits;
                        entry->xonxoff                  = xonxoff;
                        entry->rtscts                   = rtscts;
                        entry->lock_dte_baud            = lock_dte_baud;
#endif /* Q_NO_SERIAL */
                        entry->use_default_toggles      = use_default_toggles;
                        entry->toggles                  = toggles;

                        /* Save settings ----------------------------------- */

                        fieldset_free(edit_form);
                        screen_delwin(form_window);
                        Xfree(password_stars, __FILE__, __LINE__);

                        /* No leak */
                        if (name != NULL) {
                                Xfree(name, __FILE__, __LINE__);
                        }
                        if (address != NULL) {
                                Xfree(address, __FILE__, __LINE__);
                        }
                        if (port != NULL) {
                                Xfree(port, __FILE__, __LINE__);
                        }
                        if (password != NULL) {
                                Xfree(password, __FILE__, __LINE__);
                        }
                        if (username != NULL) {
                                Xfree(username, __FILE__, __LINE__);
                        }
                        if (script_filename != NULL) {
                                Xfree(script_filename, __FILE__, __LINE__);
                        }
                        if (capture_filename != NULL) {
                                Xfree(capture_filename, __FILE__, __LINE__);
                        }
                        if (keybindings_filename != NULL) {
                                Xfree(keybindings_filename, __FILE__, __LINE__);
                        }

                        return;

                case '\\':
                        /* Alt-\ Compose key */
                        if (flags & KEY_FLAG_ALT) {
                                if (must_use_picklist == Q_FALSE) {
                                        new_keystroke = compose_key(Q_TRUE);
                                        if (new_keystroke > 0) {
                                                /* Pass normal keys to form driver */
                                                if (q_key_code_yes(new_keystroke) == 0) {
                                                        fieldset_keystroke(edit_form, new_keystroke);
                                                }
                                        }
                                        dont_reload = Q_TRUE;
                                        local_dirty = Q_TRUE;
                                        real_dirty = Q_TRUE;
                                }
                        }
                        break;
                default:
                        /*
                         * This code is also copied in the case ' ' above
                         * case F2.  Make sure any changes here are reflected
                         * there too.
                         */
                        if (!q_key_code_yes(keystroke)) {
                                if (must_use_picklist == Q_FALSE) {
                                        /* Pass normal keys to form driver */
                                        fieldset_keystroke(edit_form, keystroke);
                                }
                        }
                        break;
                }
        } /* for (;;) */

        /* Should never get here. */
        return;
} /* ---------------------------------------------------------------------- */

/*
 * phonebook_keyboard_handler
 */
void phonebook_keyboard_handler(const int keystroke, const int flags) {
        struct q_phone_struct * entry;
        Q_BOOL wrapped_around;
        int delete_flag;
        SORT_METHOD sort_method;
        wchar_t * notes_line;
        int current_notes_idx;
        struct file_info * phonebook_file_info;
        int i;
        char * pick_string;
        int visible_entries_n = HEIGHT - 1 - 14;
        int new_phonebook_entry_i;
        int new_phonebook_page;
        static wchar_t * search_string = NULL;
        char * filename;
        char * print_destination = NULL;
        int new_keystroke;

        switch (keystroke) {

        case Q_KEY_F(1):
                launch_help(Q_HELP_PHONEBOOK);

                /*
                 * Refresh the whole screen.  This is one of the few screens
                 * that is "top level" where just a q_screen_dirty is enough.
                 */
                q_screen_dirty = Q_TRUE;
                break;

        case '`':
                /* Backtick works too */
        case KEY_ESCAPE:
                /* ESC return to TERMINAL mode */
                switch_state(Q_STATE_CONSOLE);

                /* The ABORT exit point */

                /* Save phonebook */
                save_phonebook(Q_FALSE);
                return;

        case 'i':
        case 'I':
        case Q_KEY_IC:
                entry = (struct q_phone_struct *)Xmalloc(sizeof(struct q_phone_struct), __FILE__, __LINE__);
                entry->name                     = Xwcsdup(L"", __FILE__, __LINE__);
                entry->address                  = Xstrdup("", __FILE__, __LINE__);
                entry->port                     = Xstrdup("", __FILE__, __LINE__);
                entry->username                 = Xwcsdup(L"", __FILE__, __LINE__);
                entry->password                 = Xwcsdup(L"", __FILE__, __LINE__);
                entry->method                   = Q_DIAL_METHOD_SSH;
                entry->port                     = default_port(entry->method);
                entry->emulation                = Q_EMUL_VT102;
                entry->codepage                 = default_codepage(entry->emulation);
                entry->script_filename          = Xstrdup("", __FILE__, __LINE__);
                entry->capture_filename         = Xstrdup("", __FILE__, __LINE__);
                entry->keybindings_filename     = Xstrdup("", __FILE__, __LINE__);
                entry->doorway                  = Q_DOORWAY_CONFIG;
                entry->use_default_toggles      = Q_TRUE;
                entry->toggles                  = 0;
                entry->notes                    = NULL;
                entry->last_call                = 0;
                entry->times_on                 = 0;
                entry->tagged                   = Q_FALSE;
#ifndef Q_NO_SERIAL
                entry->use_modem_cfg            = Q_TRUE;
                entry->baud                     = q_modem_config.default_baud;
                entry->data_bits                = q_modem_config.default_data_bits;
                entry->parity                   = q_modem_config.default_parity;
                entry->stop_bits                = q_modem_config.default_stop_bits;
                entry->rtscts                   = q_modem_config.rtscts;
                entry->xonxoff                  = q_modem_config.xonxoff;
                entry->lock_dte_baud            = q_modem_config.lock_dte_baud;
#endif /* Q_NO_SERIAL */
                entry->quicklearn               = Q_FALSE;

                if (q_phonebook.entry_count > 0) {
                        entry->next             = q_phonebook.selected_entry;
                        entry->prev             = q_phonebook.selected_entry->prev;
                        if (q_phonebook.selected_entry->prev != NULL) {
                                q_phonebook.selected_entry->prev->next  = entry;
                        } else {
                                /* Inserting the head */
                                q_phonebook.entries                     = entry;
                        }
                        q_phonebook.selected_entry->prev        = entry;
                } else {
                        q_phonebook.entries     = entry;
                        entry->next             = NULL;
                        entry->prev             = NULL;
                }
                q_phonebook.selected_entry      = entry;
                q_phonebook.entry_count++;

                /* Fall through ... */
        case 'r':
        case 'R':
                /* Don't do anything if the phonebook is empty */
                if (q_phonebook.entry_count == 0) {
                        break;
                }
                /* Revise entry */
                q_cursor_on();
                edit_phone_entry_form(q_phonebook.selected_entry);
                q_cursor_off();
                break;

        case 'd':
        case 'D':
        case Q_KEY_DC:
                /* Don't delete anything if the phonebook is empty */
                if (q_phonebook.entry_count == 0) {
                        break;
                }

                /* Popup the delete dialog */
                q_cursor_on();
                delete_flag = delete_popup();
                q_cursor_off();

                if (delete_flag == -1) {
                        /* User aborted */
                        break;
                }

                /* Save a backup copy */
                save_phonebook(Q_TRUE);

                if (delete_flag == 1) {
                        /* Notes only */
                        entry = q_phonebook.selected_entry;
                        if (entry->notes != NULL) {
                                current_notes_idx = 0;
                                for (notes_line = entry->notes[current_notes_idx]; notes_line != NULL; current_notes_idx++, notes_line = entry->notes[current_notes_idx]) {
                                        Xfree(notes_line, __FILE__, __LINE__);
                                }
                                Xfree(entry->notes, __FILE__, __LINE__);
                                entry->notes = NULL;
                        }
                        break;
                }

                if (delete_flag == 2) {
                        entry = q_phonebook.selected_entry;

                        if (entry == q_current_dial_entry) {
                                /* Can't delete current connection */
                                notify_form(_("Can't delete current connection while Online"), 1.5);
                        } else {
                                if (entry->next != NULL) {
                                        q_phonebook.selected_entry = entry->next;
                                } else {
                                        q_phonebook.selected_entry = entry->prev;
                                        phonebook_entry_i--;
                                }

                                delete_phonebook_entry(entry);
                        }
                        break;
                }

                break;

        case 'f':
        case 'F':
                /* Find Text */
                found_note_flag = Q_FALSE;
                if (search_string != NULL) {
                        Xfree(search_string, __FILE__, __LINE__);
                        search_string = NULL;
                }
                q_cursor_on();
                search_string = pick_find_string();
                q_cursor_off();
                if (search_string == NULL) {
                        break;
                }
                /* Search for the first matching entry */
                entry = q_phonebook.entries;
                new_phonebook_entry_i = 0;
                new_phonebook_page = 0;

                while (entry != NULL) {
                        if (match_phonebook_entry(search_string, entry) == Q_TRUE) {
                                q_phonebook.selected_entry = entry;
                                phonebook_entry_i = new_phonebook_entry_i;
                                phonebook_page = new_phonebook_page;
                                break;
                        }

                        entry = entry->next;
                        if (entry == NULL) {
                                /* Text not found */
                                notify_form(_("Text not found"), 1.5);
                                break;
                        } else {
                                new_phonebook_entry_i++;
                                if (new_phonebook_entry_i % visible_entries_n == 0) {
                                        new_phonebook_page++;
                                }
                        }
                }
                /* No leak */
                Xfree(search_string, __FILE__, __LINE__);
                search_string = NULL;
                break;

        case 'a':
        case 'A':
                /* Find Again */
                found_note_flag = Q_FALSE;
                if (q_phonebook.selected_entry == NULL) {
                        /* Phonebook size should be zero */
                        break;
                }

                if (search_string == NULL) {
                        /* If this is the first search (even though it's
                         * "Find Again", go ahead and pop up the find
                         * dialog.
                         */
                        q_cursor_on();
                        search_string = pick_find_string();
                        q_cursor_off();
                        if (search_string == NULL) {
                                break;
                        }
                }

                new_phonebook_entry_i = phonebook_entry_i;
                new_phonebook_page = phonebook_page;
                entry = q_phonebook.selected_entry->next;
                if (entry == NULL) {
                        /* No more matches */
                        notify_form(_("No more matches"), 1.5);
                }

                new_phonebook_entry_i++;
                if (new_phonebook_entry_i % visible_entries_n == 0) {
                        new_phonebook_page++;
                }

                while (entry != NULL) {
                        if (match_phonebook_entry(search_string, entry) == Q_TRUE) {
                                q_phonebook.selected_entry = entry;
                                phonebook_entry_i = new_phonebook_entry_i;
                                phonebook_page = new_phonebook_page;
                                break;
                        }

                        entry = entry->next;
                        if (entry == NULL) {
                                /* No more matches */
                                notify_form(_("No more matches"), 1.5);
                                break;
                        } else {
                                new_phonebook_entry_i++;
                                if (new_phonebook_entry_i % visible_entries_n == 0) {
                                        new_phonebook_page++;
                                }
                        }
                }

                break;

#ifndef Q_NO_SERIAL
        case 'm':
        case 'M':
                /* Manual dial */
                if (q_status.online == Q_TRUE) {
                        notify_form(_("Cannot choose Manual Dial when already Online."), 1.5);
                        break;
                }

                q_cursor_on();
                pick_string = pick_manual_phone_number();
                q_cursor_off();
                if (pick_string != NULL) {

                        entry = Xmalloc(sizeof(struct q_phone_struct), __FILE__, __LINE__);
                        /* Set some defaults, the user may not like them */
                        entry->name                     = Xstring_to_wcsdup(_("Manual Call"), __FILE__, __LINE__);
                        entry->method                   = Q_DIAL_METHOD_MODEM;

                        /*
                         * This will leak, but I'm not sure how to cleanly free
                         * it.  It's not really worth going after.
                         */
                        entry->address                  = pick_string;

                        entry->port                     = "";
                        entry->username                 = L"";
                        entry->password                 = L"";
                        entry->emulation                = Q_EMUL_ANSI;
                        entry->codepage                 = default_codepage(entry->emulation);
                        entry->notes                    = NULL;
                        entry->script_filename          = "";
                        entry->capture_filename         = "";
                        entry->keybindings_filename     = "";
                        entry->doorway                  = Q_DOORWAY_CONFIG;
                        entry->use_default_toggles      = Q_TRUE;
                        entry->toggles                  = 0;
#ifndef Q_NO_SERIAL
                        entry->use_modem_cfg            = Q_TRUE;
#endif /* Q_NO_SERIAL */
                        entry->quicklearn               = Q_FALSE;

                        time(&q_dialer_start_time);
                        q_dialer_attempts = 0;
                        q_current_dial_entry = entry;
                        do_dialer();
                }
                break;
#endif /* Q_NO_SERIAL */

        case 'n':
        case 'N':
                /* Edit attached note */
                if (q_phonebook.selected_entry != NULL) {
                        /* Save a backup copy */
                        save_phonebook(Q_TRUE);
                        edit_attached_note(q_phonebook.selected_entry);
                }
                break;

        case 'l':
        case 'L':
                /* Load new phonebook */
                /* Save a copy of this one */
                save_phonebook(Q_FALSE);
                phonebook_file_info = view_directory(q_home_directory, "*.txt");
                if (phonebook_file_info != NULL) {
                        q_phonebook.filename = phonebook_file_info->name;
                        load_phonebook(Q_FALSE);
                        Xfree(phonebook_file_info, __FILE__, __LINE__);
                }
                break;

        case 'o':
        case 'O':
                /* Switch view mode */
                q_phonebook.view_mode++;
                if (q_phonebook.view_mode == Q_PHONEBOOK_VIEW_MODE_MAX) {
                        q_phonebook.view_mode = 0;
                }
                break;

        case 'p':
        case 'P':
        case 0x10:
                /* Don't print anything if the phonebook is empty */
                if (q_phonebook.entry_count == 0) {
                        break;
                }

                /* Popup the print dialog */
                q_cursor_on();
                print_destination = pick_print_destination();
                q_cursor_off();

                if (print_destination == NULL) {
                        /* User aborted */
                        break;
                }

                if (keystroke == 0x10) {
                        print_phonebook_132(print_destination);
                } else {
                        print_phonebook_80(print_destination);
                }

                /* No leak */
                Xfree(print_destination, __FILE__, __LINE__);
                print_destination = NULL;
                break;

        case 'q':
        case 'Q':
                /* Q - QuickLearn */
                if (q_phonebook.selected_entry == NULL) {
                        /* Phonebook size should be zero */
                        break;
                }
                if (q_phonebook.selected_entry->tagged == Q_TRUE) {
                        q_phonebook.selected_entry->tagged = Q_FALSE;
                        q_phonebook.selected_entry->quicklearn = Q_FALSE;
                        q_phonebook.tagged--;
                } else {
                        assert(q_phonebook.selected_entry->script_filename != NULL);
                        if (q_phonebook.selected_entry->quicklearn == Q_TRUE) {
                                q_phonebook.selected_entry->quicklearn = Q_FALSE;
                        } else {
                                if (strlen(q_phonebook.selected_entry->script_filename) == 0) {
                                        notify_form(_("Script file must be specified to QuickLearn"), 1.5);
                                        break;
                                }
                                if (file_exists(get_scriptdir_filename(q_phonebook.selected_entry->script_filename)) == Q_TRUE) {
                                        /* Prompt to overwrite */
                                        new_keystroke = tolower(notify_prompt_form(
                                                _("Script File Already Exists"),
                                                _(" Overwrite File? [Y/n] "),
                                                _(" Y-Overwrite Script File   N-Do Not Quicklearn "),
                                                Q_TRUE,
                                                0.0, "YyNn\r"));
                                        if ((new_keystroke == 'y') || (new_keystroke == C_CR)) {
                                                q_phonebook.selected_entry->quicklearn = Q_TRUE;
                                                q_phonebook.selected_entry->tagged = Q_TRUE;
                                                q_phonebook.tagged++;
                                        }
                                } else {
                                        q_phonebook.selected_entry->quicklearn = Q_TRUE;
                                        q_phonebook.selected_entry->tagged = Q_TRUE;
                                        q_phonebook.tagged++;
                                }
                        }
                }
                /* Advance to the next entry */
                if (q_phonebook.selected_entry->next != NULL) {
                        q_phonebook.selected_entry = q_phonebook.selected_entry->next;
                        phonebook_normalize();
                }
                break;

        case 's':
        case 'S':
                /* Sort popup */
                sort_method = pick_sort();
                if (sort_method != -1) {
                        /* Save a backup copy */
                        save_phonebook(Q_TRUE);
                        sort_phonebook(sort_method);
                }
                break;

        case 't':
        case 'T':
                /* Tag Multiple */
                q_cursor_on();
                pick_string = pick_tag_string();
                q_cursor_off();
                if (pick_string != NULL) {
                        tag_multiple(pick_string);
                        Xfree(pick_string, __FILE__, __LINE__);
                }
                break;

        case 'u':
        case 'U':
                /* Untag all */
                for (entry=q_phonebook.entries; entry != NULL; entry = entry->next) {
                        if (entry->tagged == Q_TRUE) {
                                entry->tagged = Q_FALSE;
                        }
                }
                q_phonebook.tagged = 0;
                break;

        case 'v':
        case 'V':
                /* Pull up external editor */
                if (q_phonebook.selected_entry != NULL) {
                        filename = Xstrdup(q_phonebook.selected_entry->script_filename, __FILE__, __LINE__);
                        for (i = 0; i < strlen(filename); i++) {
                                if (filename[i] == ' ') {
                                        filename[i] = 0x0;
                                }
                        }
                        spawn_script_editor(filename);
                        Xfree(filename, __FILE__, __LINE__);
                }
                break;

        case ' ':
                /* Tag/Untag */
                if (q_phonebook.selected_entry != NULL) {
                        if (q_phonebook.selected_entry->tagged == Q_TRUE) {
                                q_phonebook.selected_entry->tagged = Q_FALSE;
                                q_phonebook.selected_entry->quicklearn = Q_FALSE;
                                q_phonebook.tagged--;
                        } else {
                                q_phonebook.selected_entry->tagged = Q_TRUE;
                                q_phonebook.tagged++;
                                if (strlen(q_phonebook.selected_entry->script_filename) > 0) {
                                        if (file_exists(get_scriptdir_filename(q_phonebook.selected_entry->script_filename)) == Q_FALSE) {
                                                /* Automatically QuickLearn on new scripts */
                                                q_phonebook.selected_entry->quicklearn = Q_TRUE;
                                        }
                                }
                        }
                        /* Advance to the next entry */
                        if (q_phonebook.selected_entry->next != NULL) {
                                q_phonebook.selected_entry = q_phonebook.selected_entry->next;
                                phonebook_normalize();
                        }
                }
                break;

        case 0x04:
                /* CTRL-D */

                /* Popup the delete dialog */
                q_cursor_on();
                delete_flag = delete_popup();
                q_cursor_off();

                if (delete_flag == -1) {
                        /* User aborted */
                        break;
                }

                /* Save a backup copy */
                save_phonebook(Q_TRUE);

                entry = q_phonebook.entries;
                while (entry != NULL) {
                        if (entry->tagged == Q_TRUE) {
                                if (delete_flag == 1) {
                                        /* Notes only */
                                        if (entry->notes != NULL) {
                                                current_notes_idx = 0;
                                                for (notes_line = entry->notes[current_notes_idx]; notes_line != NULL; current_notes_idx++, notes_line = entry->notes[current_notes_idx]) {
                                                        Xfree(notes_line, __FILE__, __LINE__);
                                                }
                                                Xfree(entry->notes, __FILE__, __LINE__);
                                                entry->notes = NULL;
                                        }
                                }

                                if (delete_flag == 2) {
                                        if (entry == q_current_dial_entry) {
                                                /* Can't delete current connection */
                                                notify_form(_("Can't delete current connection while Online"), 1.5);
                                        } else {
                                                /* Delete entire entry */
                                                delete_phonebook_entry(entry);
                                        }
                                }

                                /* Restart from beginning */
                                entry = q_phonebook.entries;
                                continue;
                        }
                        entry = entry->next;
                }

                q_phonebook.selected_entry = q_phonebook.entries;
                phonebook_entry_i = 0;
                phonebook_page = 0;
                break;

        case 0x12:
                /* CTRL-R */
                /* Revise tagged */
                for (entry=q_phonebook.entries; entry != NULL; entry = entry->next) {
                        if (entry->tagged == Q_TRUE) {
                                q_cursor_on();
                                edit_phone_entry_form(entry);
                                q_cursor_off();
                        }
                }
                break;

        case 0x15:
                /* CTRL-U */
                load_phonebook(Q_TRUE);
                break;

        case Q_KEY_UP:
                if ((q_phonebook.selected_entry != NULL) && (q_phonebook.selected_entry->prev != NULL)) {
                        q_phonebook.selected_entry = q_phonebook.selected_entry->prev;
                        phonebook_entry_i--;

                        if (phonebook_entry_i % visible_entries_n == visible_entries_n - 1) {
                                phonebook_page--;
                        }
                }

                break;

        case Q_KEY_DOWN:
                if ((q_phonebook.selected_entry != NULL) && (q_phonebook.selected_entry->next != NULL)) {
                        q_phonebook.selected_entry = q_phonebook.selected_entry->next;
                        phonebook_entry_i++;

                        if (phonebook_entry_i % visible_entries_n == 0) {
                                phonebook_page++;
                        }
                }

                break;

        case Q_KEY_PPAGE:
                /* PgUp */
                if (q_phonebook.selected_entry == NULL) {
                        /* Phonebook size should be zero */
                        break;
                }
                entry = q_phonebook.selected_entry;
                for (i=0; (i<visible_entries_n) && (entry->prev != NULL); i++) {
                        entry = entry->prev;
                }
                q_phonebook.selected_entry = entry;
                phonebook_normalize();
                break;

        case Q_KEY_NPAGE:
                /* PgDn */
                if (q_phonebook.selected_entry == NULL) {
                        /* Phonebook size should be zero */
                        break;
                }
                entry = q_phonebook.selected_entry;
                for (i=0; (i<visible_entries_n) && (entry->next != NULL); i++) {
                        entry = entry->next;
                }
                q_phonebook.selected_entry = entry;
                phonebook_normalize();
                break;

        case Q_KEY_HOME:
                /* Home */
                if (q_phonebook.selected_entry == NULL) {
                        /* Phonebook size should be zero */
                        break;
                }
                q_phonebook.selected_entry = q_phonebook.entries;
                phonebook_entry_i = 0;
                phonebook_page = 0;
                break;

        case Q_KEY_END:
                /* End */
                if (q_phonebook.selected_entry == NULL) {
                        /* Phonebook size should be zero */
                        break;
                }
                entry = q_phonebook.selected_entry;
                while (entry->next != NULL) {
                        entry = entry->next;
                }
                q_phonebook.selected_entry = entry;
                phonebook_normalize();
                break;

        case Q_KEY_ENTER:
        case C_CR:
                /* Dial */
                if (q_phonebook.selected_entry == NULL) {
                        /* Phonebook size should be zero */
                        break;
                }

                entry = q_phonebook.selected_entry;
                wrapped_around = Q_FALSE;
                for (;;) {
                        if (entry->tagged == Q_TRUE) {
                                q_phonebook.selected_entry = entry;
                                break;
                        }

                        entry = entry->next;
                        if (entry == NULL) {
                                /* Wrap around */
                                entry = q_phonebook.entries;
                                wrapped_around = Q_TRUE;
                                phonebook_entry_i = 0;
                                phonebook_page = 0;
                        } else {
                                phonebook_entry_i++;
                                if (phonebook_entry_i % visible_entries_n == 0) {
                                        phonebook_page++;
                                }
                        }


                        if ((entry == q_phonebook.selected_entry) && (wrapped_around == Q_TRUE)) {
                                /* We circled around and found no
                                   tagged entries, so break without
                                   setting a new selected_entry.  This
                                   is the normal behavior when nothing
                                   is tagged.
                                 */
                                break;
                        }
                }

                if (strlen(q_phonebook.selected_entry->script_filename) > 0) {
                        if (file_exists(get_scriptdir_filename(q_phonebook.selected_entry->script_filename)) == Q_FALSE) {
                                /* Automatically QuickLearn on new scripts */
                                q_phonebook.selected_entry->quicklearn = Q_TRUE;
                        }
                }

                if (q_status.online == Q_FALSE) {
                        time(&q_dialer_start_time);
                        q_dialer_attempts = 0;
                        q_current_dial_entry = q_phonebook.selected_entry;
                        do_dialer();
                }
                break;


        default:
                /* Ignore keystroke */
                return;
        }

        /* The OK exit point */
        q_screen_dirty = Q_TRUE;
} /* ---------------------------------------------------------------------- */

/*
 * dialer_keyboard_handler - I know, weird to have it in the phonebook,
 * but it needs access to all sorts of phonebook variables and I don't
 * want to pollute the entire namespace.
 */
void dialer_keyboard_handler(const int keystroke, const int flags) {

        /* Press ANY key to continue */
        if (q_dial_state == Q_DIAL_CONNECTED) {

                /* Time is up, switch to console */
                switch_state(Q_STATE_CONSOLE);
                q_screen_dirty = Q_TRUE;

                if (    (strlen(q_current_dial_entry->script_filename) > 0) &&
                        (file_exists(get_scriptdir_filename(q_current_dial_entry->script_filename)) == Q_TRUE)) {
                        if (q_status.quicklearn == Q_FALSE) {
                                /* Execute script if supplied */
                                script_start(q_current_dial_entry->script_filename);
                        }
                }
                return;
        }

        switch (keystroke) {

        case 'K':
        case 'k':
                /* Untag this entry and cycle to the next one */
                if (kill_redialer_number() == Q_TRUE) {
                        q_dial_state = Q_DIAL_KILLED;
                } else {
                        /* We're out of numbers to call */
                        q_dial_state = Q_DIAL_NO_NUMBERS_LEFT;
                }
                time(&q_dialer_cycle_start_time);
                close_dial_entry();
                break;

        case 'C':
        case 'c':
                /* Cycle to the next entry */
                time(&q_dialer_cycle_start_time);
                if (    (q_dial_state != Q_DIAL_MANUAL_CYCLE) &&
                        (q_dial_state != Q_DIAL_BETWEEN_PAUSE)
                ) {
                        q_dial_state = Q_DIAL_MANUAL_CYCLE;
                } else {
                        q_dialer_cycle_start_time -= atoi(get_option(Q_OPTION_DIAL_BETWEEN_TIME));;
                }
                close_dial_entry();
                break;

        case 'X':
        case 'x':
                /* Add 10 seconds to the timeout */
                q_dialer_cycle_start_time += 10;
                break;

        case '`':
                /* Backtick works too */
        case KEY_ESCAPE:
                if ((q_dial_state == Q_DIAL_NO_NUMBERS_LEFT) || (q_dial_state == Q_DIAL_USER_ABORTED)) {
                        close_dial_entry();
                        q_current_dial_entry = NULL;
                        switch_state(Q_STATE_PHONEBOOK);
                        q_screen_dirty = Q_TRUE;
                        /*
                         * We need to explicitly call refresh_hanlder() because
                         * phonebook_keyboard_handler() blocks.
                         */
                        refresh_handler();
                } else {
                        q_dial_state = Q_DIAL_USER_ABORTED;
                        time(&q_dialer_cycle_start_time);
                }
                /* The ABORT exit point */
                return;

        default:
                /* Ignore keystroke */
                return;
        }

} /* ---------------------------------------------------------------------- */

#ifndef Q_NO_SERIAL

/*
 * Process data during a MODEM connection attempt
 */
static void modem_data(unsigned char * input, int input_n, int * remaining, unsigned char * output, int * output_n, const int output_max) {
        int i;
        Q_BOOL complete_line = Q_FALSE;
        int new_dce_baud;
        int unused;
        char * begin = (char *)input;
        int menu_left = 1 + (WIDTH - 80)/2;
        int menu_top = HEIGHT - 1 - 9;

#ifdef DEBUG_DIALER
        fprintf(DEBUG_FILE_HANDLE, "modem_data()\n");
        fflush(DEBUG_FILE_HANDLE);
#endif /* DEBUG_DIALER */

        if (modem_state == DIAL_MODEM_CONNECTED) {
#ifdef DEBUG_DIALER
                fprintf(DEBUG_FILE_HANDLE, "modem_data() DIAL_MODEM_CONNECTED\n");
                fflush(DEBUG_FILE_HANDLE);
#endif /* DEBUG_DIALER */
                if (    (strlen(q_current_dial_entry->script_filename) > 0) &&
                        (file_exists(get_scriptdir_filename(q_current_dial_entry->script_filename)) == Q_TRUE)) {
                        if (q_status.quicklearn == Q_FALSE) {
                                /* Execute script if supplied */
                                script_start(q_current_dial_entry->script_filename);
                        }
                }

                /* We got some data.  Switch to console mode and process it */
                switch_state(Q_STATE_CONSOLE);
                q_screen_dirty = Q_TRUE;
                console_process_incoming_data(input, input_n, &unused);
                return;
        }

        /* Break up whatever is coming in into separate lines */
        for (i = 0; i < input_n; i++) {
                if (input[i] == '\n') {
                        /* Ignore line feeds */
                        input[i] = 0;
                }
        }
        while ((*remaining > 0) && ((begin[0] == 0) || isspace(begin[0]))) {
                begin++;
                *remaining -= 1;
                if (*remaining == 0) {
                        return;
                }
        }

#ifdef DEBUG_DIALER
        fprintf(DEBUG_FILE_HANDLE, "modem_data() %d input after stripping leading whitespace: ", input_n);
        for (i = 0; i < *remaining; i++) {
                fprintf(DEBUG_FILE_HANDLE, "%02x ", begin[i]);
        }
        fprintf(DEBUG_FILE_HANDLE, " | \"");
        for (i = 0; i < *remaining; i++) {
                fprintf(DEBUG_FILE_HANDLE, "%c", begin[i]);
        }
        fprintf(DEBUG_FILE_HANDLE, "\"\n");
        fflush(DEBUG_FILE_HANDLE);
#endif /* DEBUG_DIALER */

        for (i = 0; i < *remaining; i++) {
                if (begin[i] == '\r') {
                        /* Break on carriage return */
                        complete_line = Q_TRUE;
                        begin[i] = 0;

                        /* Clear UI line */
                        memset(q_dialer_modem_message, 0, sizeof(q_dialer_modem_message));

                        /* Copy what is here to it */
                        snprintf(q_dialer_modem_message, sizeof(q_dialer_modem_message),
                                "%s", begin);

                        /* Stop looking for a line terminator */
                        break;
                }
        }

#ifdef DEBUG_DIALER
        fprintf(DEBUG_FILE_HANDLE, "modem_data() %d input after looking for CR: ", *remaining);
        for (i = 0; i < *remaining; i++) {
                fprintf(DEBUG_FILE_HANDLE, "%02x ", begin[i]);
        }
        fprintf(DEBUG_FILE_HANDLE, " | \"");
        for (i = 0; i < *remaining; i++) {
                fprintf(DEBUG_FILE_HANDLE, "%c", begin[i]);
        }
        fprintf(DEBUG_FILE_HANDLE, "\"\n");
        fflush(DEBUG_FILE_HANDLE);
#endif /* DEBUG_DIALER */

#ifdef DEBUG_DIALER
        fprintf(DEBUG_FILE_HANDLE, "modem_data() q_dialer_modem_message = \"%s\"\n", q_dialer_modem_message);
        fflush(DEBUG_FILE_HANDLE);
#endif /* DEBUG_DIALER */

        switch (modem_state) {
        case DIAL_MODEM_INIT:
#ifdef DEBUG_DIALER
                fprintf(DEBUG_FILE_HANDLE, "modem_data() MODEM_INIT\n");
                fflush(DEBUG_FILE_HANDLE);
#endif /* DEBUG_DIALER */

                /* Send AT, expect OK */
                snprintf((char *)output, output_max, "AT\r");
                *output_n = strlen((char *)output);
                /* Clear modem message */
                memset(q_dialer_modem_message, 0, sizeof(q_dialer_modem_message));
                modem_state = DIAL_MODEM_SENT_AT;
                break;

        case DIAL_MODEM_SENT_AT:
#ifdef DEBUG_DIALER
                fprintf(DEBUG_FILE_HANDLE, "modem_data() MODEM_SENT_AT\n");
                fflush(DEBUG_FILE_HANDLE);
#endif /* DEBUG_DIALER */

                if (complete_line == Q_TRUE) {
                        /* Expect OK */
                        if (strcasecmp(q_dialer_modem_message, "at") == 0) {
                                /* Modem echo is on, just discard this part */

                                /* Clear modem message */
                                memset(q_dialer_modem_message, 0, sizeof(q_dialer_modem_message));

                                /* Toss the input seen so far */
                                *remaining -= strlen(begin);
                        }

                        /* Expect OK */
                        if (strcasecmp(q_dialer_modem_message, "ok") == 0) {
#ifdef DEBUG_DIALER
                                fprintf(DEBUG_FILE_HANDLE, "modem_data() MODEM_SENT_AT ** OK **\n");
                                fflush(DEBUG_FILE_HANDLE);
#endif /* DEBUG_DIALER */
                                /* Got OK.  Send the dial string */
                                snprintf((char *)output, output_max, "%s%s\r", q_modem_config.dial_string, q_current_dial_entry->address);
                                *output_n = strlen((char *)output);

                                /* Clear modem message */
                                memset(q_dialer_modem_message, 0, sizeof(q_dialer_modem_message));

                                /* Toss the input seen so far */
                                *remaining -= strlen(begin);

                                /* New state */
                                modem_state = DIAL_MODEM_SENT_DIAL_STRING;
                        }
                }

                break;

        case DIAL_MODEM_SENT_DIAL_STRING:
#ifdef DEBUG_DIALER
                fprintf(DEBUG_FILE_HANDLE, "modem_data() MODEM_SENT_DIAL_STRING\n");
                fflush(DEBUG_FILE_HANDLE);
#endif /* DEBUG_DIALER */

                if (complete_line == Q_TRUE) {
#ifdef DEBUG_DIALER
                        fprintf(DEBUG_FILE_HANDLE, "modem_data() MODEM_SENT_DIAL_STRING line came in: %s\n", q_dialer_modem_message);
                        fflush(DEBUG_FILE_HANDLE);
#endif /* DEBUG_DIALER */

                        /* Expect CONNECT, NO CARRIER, BUSY, or VOICE */
                        if ((strstr(q_dialer_modem_message, "NO DIALTONE") != NULL) ||
                                (strstr(q_dialer_modem_message, "BUSY") != NULL) ||
                                (strstr(q_dialer_modem_message, "NO CARRIER") != NULL) ||
                                (strstr(q_dialer_modem_message, "VOICE") != 0)) {
#ifdef DEBUG_DIALER
                                fprintf(DEBUG_FILE_HANDLE, "modem_data() MODEM_SENT_DIAL_STRING CYCLE\n");
                                fflush(DEBUG_FILE_HANDLE);
#endif /* DEBUG_DIALER */

                                /* Uh-oh, cycle */
                                q_dial_state = Q_DIAL_LINE_BUSY;
                                time(&q_dialer_cycle_start_time);
                                close_serial_port();
                        }
                        if ((strstr(q_dialer_modem_message, q_modem_config.dial_string) != NULL) ||
                                (strstr(q_dialer_modem_message, q_current_dial_entry->address) != NULL)) {
                                /*
                                 * Modem echo is on, just discard this part.
                                 * But keep it on the display.
                                 */

                                /* Toss the input seen so far */
                                *remaining -= strlen(begin);
                        }

                        if (strstr(q_dialer_modem_message, "CONNECT") != NULL) {
                                /* Yippee, connect! */
#ifdef DEBUG_DIALER
                                fprintf(DEBUG_FILE_HANDLE, "modem_data() MODEM_SENT_DIAL_STRING *** CONNECT ***\n");
                                fflush(DEBUG_FILE_HANDLE);
#endif /* DEBUG_DIALER */

                                /* Find baud */
#ifdef DEBUG_DIALER
                                fprintf(DEBUG_FILE_HANDLE, "q_dialer_modem_message \'%s\'", q_dialer_modem_message);
#endif /* DEBUG_DIALER */

                                if (sscanf(q_dialer_modem_message, "CONNECT %d", &new_dce_baud) == 1) {
                                        q_serial_port.dce_baud = new_dce_baud;

                                        if (q_serial_port.lock_dte_baud == Q_FALSE) {
                                                /* Change DTE baud rate */
                                                if (q_serial_port.dce_baud <= 300) {
                                                        q_serial_port.baud = Q_BAUD_300;
                                                } else if (q_serial_port.dce_baud <= 1200) {
                                                        q_serial_port.baud = Q_BAUD_1200;
                                                } else if (q_serial_port.dce_baud <= 2400) {
                                                        q_serial_port.baud = Q_BAUD_2400;
                                                } else if (q_serial_port.dce_baud <= 4800) {
                                                        q_serial_port.baud = Q_BAUD_4800;
                                                } else if (q_serial_port.dce_baud <= 9600) {
                                                        q_serial_port.baud = Q_BAUD_9600;
                                                } else if (q_serial_port.dce_baud <= 19200) {
                                                        q_serial_port.baud = Q_BAUD_19200;
                                                } else if (q_serial_port.dce_baud <= 38400) {
                                                        q_serial_port.baud = Q_BAUD_38400;
                                                } else if (q_serial_port.dce_baud <= 57600) {
                                                        q_serial_port.baud = Q_BAUD_57600;
                                                } else if (q_serial_port.dce_baud <= 115200) {
                                                        q_serial_port.baud = Q_BAUD_115200;
                                                } else {
                                                        q_serial_port.baud = Q_BAUD_115200;
                                                }
                                                configure_serial_port();
                                        }
                                }
                                dial_success();

                                /*
                                 * Set the status message here.  This is
                                 * ripped directly out of
                                 * phonebook_refresh().
                                 */
                                sprintf(q_dialer_status_message, _("CONNECTED, press a key to continue"));
                                screen_put_color_hline_yx(menu_top + 5, menu_left + 2, ' ', 55, Q_COLOR_MENU_TEXT);
                                screen_put_color_str_yx(menu_top + 5, menu_left + 2, _("Modem  : "), Q_COLOR_MENU_TEXT);
                                screen_put_color_str(q_dialer_modem_message, Q_COLOR_MENU_COMMAND);
                                screen_put_color_str_yx(menu_top + 6, menu_left + 2, _("Status : "), Q_COLOR_MENU_TEXT);
                                screen_put_color_str(q_dialer_status_message, Q_COLOR_MENU_COMMAND);
                                screen_flush();

                                modem_state = DIAL_MODEM_CONNECTED;
                                time(&q_dialer_cycle_start_time);

                                qlog(_("CONNECTION ESTABLISHED: %s baud\n"),
                                        q_serial_port.dce_baud);

                                /* Play connect sequence */
                                if (q_status.beeps == Q_TRUE) {
                                        play_sequence(Q_MUSIC_CONNECT_MODEM);
                                }
                        }

                }
                break;

        case DIAL_MODEM_CONNECTED:
                assert(1 == 0);
        }


} /* ---------------------------------------------------------------------- */

#endif /* Q_NO_SERIAL */

/*
 * Process data during a connection attempt
 */
void dialer_process_data(unsigned char * input, const int input_n, int * remaining, unsigned char * output, int * output_n, const int output_max) {

#ifdef DEBUG_DIALER
        if (DEBUG_FILE_HANDLE == NULL) {
                DEBUG_FILE_HANDLE = fopen("debug_dialer.txt", "w");
        }
        fflush(DEBUG_FILE_HANDLE);
#endif /* DEBUG_DIALER */

#ifdef DEBUG_DIALER
        int i;
        fprintf(DEBUG_FILE_HANDLE, "DIALER: %d input bytes:  ", input_n);
        for (i = 0; i < input_n; i++) {
                fprintf(DEBUG_FILE_HANDLE, "%02x ", input[i]);
        }
        fprintf(DEBUG_FILE_HANDLE, " | \"");
        for (i = 0; i < input_n; i++) {
                fprintf(DEBUG_FILE_HANDLE, "%c", input[i]);
        }
        fprintf(DEBUG_FILE_HANDLE, "\"\n");
        fflush(DEBUG_FILE_HANDLE);
#endif /* DEBUG_DIALER */

        switch (q_current_dial_entry->method) {

#ifndef Q_NO_SERIAL
        case Q_DIAL_METHOD_MODEM:
                /* Go through the modem states */
                modem_data(input, input_n, remaining, output, output_n, output_max);
                break;
#endif /* Q_NO_SERIAL */

        case Q_DIAL_METHOD_SSH:
        case Q_DIAL_METHOD_RLOGIN:
        case Q_DIAL_METHOD_TELNET:
        case Q_DIAL_METHOD_SOCKET:
                /*
                 * Do nothing.  We got here for the 1-2 seconds that we are
                 * displaying the CONNECTED message on the phonebook redialer
                 * window.
                 */
                break;
        /* case Q_DIAL_METHOD_TN3270: */
        case Q_DIAL_METHOD_SHELL:
        case Q_DIAL_METHOD_COMMANDLINE:
                /* BUG - these go straight to CONSOLE */
                assert(1 == 0);
        }

#ifdef DEBUG_DIALER
        fprintf(DEBUG_FILE_HANDLE, "DIALER: EXITING %d input bytes:  ", *remaining);
        for (i = input_n - *remaining; i < input_n; i++) {
                fprintf(DEBUG_FILE_HANDLE, "%02x ", input[i]);
        }
        fprintf(DEBUG_FILE_HANDLE, " | \"");
        for (i = input_n - *remaining; i < input_n; i++) {
                fprintf(DEBUG_FILE_HANDLE, "%c", input[i]);
        }
        fprintf(DEBUG_FILE_HANDLE, "\"\n");
        fflush(DEBUG_FILE_HANDLE);
#endif /* DEBUG_DIALER */


#ifdef DEBUG_DIALER
        fprintf(DEBUG_FILE_HANDLE, "DIALER: %d output bytes: ", *output_n);
        for (i = 0; i < *output_n; i++) {
                fprintf(DEBUG_FILE_HANDLE, "%02x ", output[i]);
        }
        fprintf(DEBUG_FILE_HANDLE, " | \"");
        for (i = 0; i < *output_n; i++) {
                fprintf(DEBUG_FILE_HANDLE, "%c", output[i]);
        }
        fprintf(DEBUG_FILE_HANDLE, "\"\n");
        fprintf(DEBUG_FILE_HANDLE, "\n");
        fflush(DEBUG_FILE_HANDLE);
#endif /* DEBUG_DIALER */

} /* ---------------------------------------------------------------------- */