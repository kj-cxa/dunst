/* copyright 2013 Sascha Kruse and contributors (see LICENSE for licensing information) */

#include "option_parser.h"

#include <glib.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "x11/x.h"
#include "dunst.h"
#include "log.h"
#include "utils.h"
#include "settings.h"
#include "rules.h"
#include "settings_data.h"

struct entry {
        char *key;
        char *value;
};

struct section {
        char *name;
        int entry_count;
        struct entry *entries;
};

static int section_count = 0;
static struct section *sections;

static struct section *new_section(const char *name);
static struct section *get_section(const char *name);
static void add_entry(const char *section_name, const char *key, const char *value);
static const char *get_value(const char *section, const char *key);

static int cmdline_argc;
static char **cmdline_argv;

static char *usage_str = NULL;
static void cmdline_usage_append(const char *key, const char *type, const char *description);

static int cmdline_find_option(const char *key);

#define STRING_PARSE_RET(string, value) if (STR_EQ(s, string)) { *ret = value; return true; }

int string_parse_enum(void *data, const char *s, void * ret) {
        struct string_to_enum_def *string_to_enum = (struct string_to_enum_def*)data;
        for (int i = 0; string_to_enum[i].string != NULL; i++) {
                if (strcmp(s, string_to_enum[i].string) == 0){
                        *(int*) ret = string_to_enum[i].enum_value;
                        LOG_D("Setting enum to %i (%s)", *(int*) ret, string_to_enum[i].string);
                        return true;
                }
        }
        return false;
}

int string_parse_mouse_action_list(char **s, void *ret_void)
{
        enum mouse_action **ret = (enum mouse_action **) ret_void;
        ASSERT_OR_RET(s, false);
        ASSERT_OR_RET(ret, false);

        int len = 0;
        while (s[len])
                len++;

        g_free(*ret);
        *ret = g_malloc_n((len + 1), sizeof(enum mouse_action));
        for (int i = 0; i < len; i++) {
                if (!string_parse_enum(&mouse_action_enum_data, s[i], *ret + i)) {
                        LOG_W("Unknown mouse action value: '%s'", s[i]);
                        g_free(*ret);
                        return false;
                }
        }
        (*ret)[len] = -1; // sentinel end value
        return true;
}

int string_parse_list(void *data, const char *s, void *ret) {
        enum list_type type = *(int*) data;
        char **arr = string_to_array(s);
        int success = false;
        switch (type) {
                case MOUSE_LIST:
                        success = string_parse_mouse_action_list(arr, ret);
                        break;
                default:
                        LOG_W("Don't know this list type: %i", type);
                        break;
        }
        g_strfreev(arr);
        return success;
}

int string_parse_sepcolor(void *data, const char *s, void *ret)
{
        LOG_D("parsing sep_color");
        struct separator_color_data *sep_color = (struct separator_color_data*) ret;

        enum separator_color type;
        bool is_enum = string_parse_enum(data, s, &type);
        if (is_enum) {
                sep_color->type = type;
                return true;
        } else {
                if (STR_FULL(s)) {
                        sep_color->type = SEP_CUSTOM;
                        g_free(sep_color->sep_color);
                        sep_color->sep_color = g_strdup(s); // TODO check if this is a color?
                        return true;
                } else {
                        LOG_W("Sep color is empty, make sure to quote the value if it's a color.");
                        return false;
                }
        }
}

bool string_parse_fullscreen(const char *s, enum behavior_fullscreen *ret)
{
        ASSERT_OR_RET(STR_FULL(s), false);
        ASSERT_OR_RET(ret, false);

        STRING_PARSE_RET("show",     FS_SHOW);
        STRING_PARSE_RET("delay",    FS_DELAY);
        STRING_PARSE_RET("pushback", FS_PUSHBACK);

        return false;
}

bool string_parse_markup_mode(const char *s, enum markup_mode *ret)
{
        ASSERT_OR_RET(STR_FULL(s), false);
        ASSERT_OR_RET(ret, false);

        STRING_PARSE_RET("strip", MARKUP_STRIP);
        STRING_PARSE_RET("no",    MARKUP_NO);
        STRING_PARSE_RET("full",  MARKUP_FULL);
        STRING_PARSE_RET("yes",   MARKUP_FULL);

        return false;
}

bool string_parse_urgency(const char *s, enum urgency *ret)
{
        ASSERT_OR_RET(STR_FULL(s), false);
        ASSERT_OR_RET(ret, false);

        STRING_PARSE_RET("low",      URG_LOW);
        STRING_PARSE_RET("normal",   URG_NORM);
        STRING_PARSE_RET("critical", URG_CRIT);

        return false;
}

struct section *new_section(const char *name)
{
        for (int i = 0; i < section_count; i++) {
                if (STR_EQ(name, sections[i].name)) {
                        DIE("Duplicated section in dunstrc detected.");
                }
        }

        section_count++;
        sections = g_realloc(sections, sizeof(struct section) * section_count);
        sections[section_count - 1].name = g_strdup(name);
        sections[section_count - 1].entries = NULL;
        sections[section_count - 1].entry_count = 0;
        return &sections[section_count - 1];
}

void free_ini(void)
{
        for (int i = 0; i < section_count; i++) {
                for (int j = 0; j < sections[i].entry_count; j++) {
                        g_free(sections[i].entries[j].key);
                        g_free(sections[i].entries[j].value);
                }
                g_free(sections[i].entries);
                g_free(sections[i].name);
        }
        g_clear_pointer(&sections, g_free);
        section_count = 0;
}

struct section *get_section(const char *name)
{
        for (int i = 0; i < section_count; i++) {
                if (STR_EQ(sections[i].name, name))
                        return &sections[i];
        }

        return NULL;
}

void add_entry(const char *section_name, const char *key, const char *value)
{
        struct section *s = get_section(section_name);
        if (!s)
                s = new_section(section_name);

        s->entry_count++;
        int len = s->entry_count;
        s->entries = g_realloc(s->entries, sizeof(struct entry) * len);
        s->entries[s->entry_count - 1].key = g_strdup(key);
        s->entries[s->entry_count - 1].value = string_strip_quotes(value);
}

const char *get_value(const char *section, const char *key)
{
        struct section *s = get_section(section);
        ASSERT_OR_RET(s, NULL);

        for (int i = 0; i < s->entry_count; i++) {
                if (STR_EQ(s->entries[i].key, key)) {
                        return s->entries[i].value;
                }
        }
        return NULL;
}

char *ini_get_path(const char *section, const char *key, const char *def)
{
        return string_to_path(ini_get_string(section, key, def));
}

char *ini_get_string(const char *section, const char *key, const char *def)
{
        const char *value = get_value(section, key);
        if (value)
                return g_strdup(value);

        return def ? g_strdup(def) : NULL;
}

gint64 ini_get_time(const char *section, const char *key, gint64 def)
{
        const char *timestring = get_value(section, key);
        gint64 val = def;

        if (timestring) {
                val = string_to_time(timestring);
        }

        return val;
}

char **ini_get_list(const char *section, const char *key, const char *def)
{
        const char *value = get_value(section, key);
        if (value)
                return string_to_array(value);
        else
                return string_to_array(def);
}

int ini_get_int(const char *section, const char *key, int def)
{
        const char *value = get_value(section, key);
        if (value)
                return atoi(value);
        else
                return def;
}

double ini_get_double(const char *section, const char *key, double def)
{
        const char *value = get_value(section, key);
        if (value)
                return atof(value);
        else
                return def;
}

bool ini_is_set(const char *ini_section, const char *ini_key)
{
        return get_value(ini_section, ini_key) != NULL;
}

const char *next_section(const char *section)
{
        ASSERT_OR_RET(section_count > 0, NULL);
        ASSERT_OR_RET(section, sections[0].name);

        for (int i = 0; i < section_count; i++) {
                if (STR_EQ(section, sections[i].name)) {
                        if (i + 1 >= section_count)
                                return NULL;
                        else
                                return sections[i + 1].name;
                }
        }
        return NULL;
}

int str_to_bool(const char *value){
        int def = -1;
        if (value) {
                switch (value[0]) {
                case 'y':
                case 'Y':
                case 't':
                case 'T':
                case '1':
                        return true;
                case 'n':
                case 'N':
                case 'f':
                case 'F':
                case '0':
                        return false;
                default:
                        return def;
                }
        } else {
                return def;
        }
}

int ini_get_bool(const char *section, const char *key, int def)
{
        const char *value = get_value(section, key);
        int val_int = str_to_bool(value);

        if (val_int < 0) val_int = def;
        return val_int;
}

int get_setting_id(const char *key, const char *section) {
        int error_code = 0;
        int partial_match_id = -1;
        for (int i = 0; i < G_N_ELEMENTS(allowed_settings); i++) {
                if (strcmp(allowed_settings[i].name, key) == 0) {
                        /* LOG_I("%s name exists with id %i", allowed_settings[i].name, i); */
                        if (strcmp(section, allowed_settings[i].section) == 0) {
                                return i;
                        } else {
                                // name matches, but in wrong section. Continueing to see
                                // if we find the same setting name with another section
                                error_code = -2;
                                partial_match_id = i;
                                continue;
                        }
                }
        }

        if (error_code == -2) {
                LOG_W("Setting %s is in the wrong section (%s, should be %s)", // TODO fix this warning
                                key, section,
                                allowed_settings[partial_match_id].section);
                // found, but in wrong section
                return -2;
        }

        // not found
        return -1;
}

bool set_setting(struct setting setting, char* value) {
        // TODO make sure value isn't empty
        LOG_D("Trying to set %s to %s", setting.name, value);
        GError *error = NULL;
        /* if (!strlen(value)) { */
                /* LOG_W("but value is empty!"); */
                /* return false; */
        /* } */
        switch (setting.type) {
                case TYPE_INT:
                        *(int*) setting.value = atoi(value);
                        return true;
                case TYPE_BOOLEAN:
                        *(bool*) setting.value = str_to_bool(value);
                        return true;
                case TYPE_STRING:
                        g_free(*(char**) setting.value);
                        *(char**) setting.value = g_strdup(value);
                        return true;
                case TYPE_ENUM:
                        if (setting.parser == NULL) {
                                LOG_W("Enum setting %s doesn't have parser", setting.name);
                                return false;
                        }
                        bool success = setting.parser(setting.parser_data, value, setting.value);

                        if (!success) LOG_W("Unknown %s value: '%s'", setting.name, value);
                        return success;
                case TYPE_SEP_COLOR:
                        if (setting.parser == NULL) {
                                LOG_W("Setting %s doesn't have parser", setting.name);
                                return false;
                        }
                        bool success2 = setting.parser(setting.parser_data, value, setting.value);

                        if (!success2) LOG_W("Unknown %s value: '%s'", setting.name, value);
                        return success2;
                case TYPE_PATH: ;
                        g_free(*(char**) setting.value);
                        *(char**) setting.value = string_to_path(g_strdup(value));
                        g_strfreev(*(char***)setting.parser_data);
                        if (!g_shell_parse_argv(*(char**) setting.value, NULL, (char***)setting.parser_data, &error)) {
                                LOG_W("Unable to parse %s command: '%s'. "
                                                "It's functionality will be disabled.",
                                                setting.name, error->message);
                                g_error_free(error);
                                return false;
                        }
                        return true;
                case TYPE_TIME: ;
                        *(gint64*) setting.value = string_to_time(value);
                        return true;
                case TYPE_GEOMETRY:
                        *(struct geometry*) setting.value = x_parse_geometry(value);
                        return true;
                case TYPE_LIST: ;
                        int type = *(enum list_type*)setting.parser_data;
                        LOG_D("list type %i", *(int*)setting.parser_data);
                        LOG_D("list type int %i", type);
                        return string_parse_list(&type, value, setting.value);
                default:
                        LOG_W("Setting type of '%s' is not known (type %i)", setting.name, setting.type);
                        return false;
        }
}

void set_defaults(){
        for (int i = 0; i < G_N_ELEMENTS(allowed_settings); i++) {
                if(!set_setting(allowed_settings[i], allowed_settings[i].default_value)) {
                        LOG_E("Could not set default of setting %s", allowed_settings[i].name);
                }
        }
}

bool is_special_section(const struct section s) {
        for (size_t i = 0; i < G_N_ELEMENTS(special_sections); i++) {
                if (STR_EQ(special_sections[i], s.name)) {
                        return true;
                }
        }
        return false;
}

void save_settings() {
        for (int i = 0; i < section_count; i++) {
                const struct section curr_section = sections[i];
                if (is_special_section(curr_section)) {
                        // special section, so don't interpret as rule
                        for (int j = 0; j < curr_section.entry_count; j++) {
                                const struct entry curr_entry = curr_section.entries[j];
                                int setting_id = get_setting_id(curr_entry.key, curr_section.name);
                                if (setting_id < 0){
                                        if (setting_id == -1) {
                                                LOG_W("Setting %s in section %s doesn't exist", curr_entry.key, curr_section.name);
                                        }
                                        continue;
                                }

                                struct setting curr_setting = allowed_settings[setting_id];
                                set_setting(curr_setting, curr_entry.value);
                        }
                } else {
                        // interpret this section as a rule
                }
        }
}

int load_ini_file(FILE *fp)
{
        ASSERT_OR_RET(fp, 1);

        char *line = NULL;
        size_t line_len = 0;

        int line_num = 0;
        char *current_section = NULL;
        while (getline(&line, &line_len, fp) != -1) {
                line_num++;

                char *start = g_strstrip(line);

                if (*start == ';' || *start == '#' || STR_EMPTY(start))
                        continue;

                if (*start == '[') {
                        char *end = strchr(start + 1, ']');
                        if (!end) {
                                LOG_W("Invalid config file at line %d: Missing ']'.", line_num);
                                continue;
                        }

                        *end = '\0';

                        g_free(current_section);
                        current_section = (g_strdup(start + 1));
                        new_section(current_section);
                        continue;
                }

                char *equal = strchr(start + 1, '=');
                if (!equal) {
                        LOG_W("Invalid config file at line %d: Missing '='.", line_num);
                        continue;
                }

                *equal = '\0';
                char *key = g_strstrip(start);
                char *value = g_strstrip(equal + 1);

                char *quote = strchr(value, '"');
                char *value_end = NULL;
                if (quote) {
                        value_end = strchr(quote + 1, '"');
                        if (!value_end) {
                                LOG_W("Invalid config file at line %d: Missing '\"'.", line_num);
                                continue;
                        }
                } else {
                        value_end = value;
                }

                char *comment = strpbrk(value_end, "#;");
                if (comment)
                        *comment = '\0';

                value = g_strstrip(value);

                if (!current_section) {
                        LOG_W("Invalid config file at line %d: Key value pair without a section.", line_num);
                        continue;
                }

                add_entry(current_section, key, value);
        }
        free(line);
        g_free(current_section);
        return 0;
}

void cmdline_load(int argc, char *argv[])
{
        cmdline_argc = argc;
        cmdline_argv = argv;
}

int cmdline_find_option(const char *key)
{
        ASSERT_OR_RET(key, -1);

        char *key1 = g_strdup(key);
        char *key2 = strchr(key1, '/');

        if (key2) {
                *key2 = '\0';
                key2++;
        }

        /* look for first key */
        for (int i = 0; i < cmdline_argc; i++) {
                if (STR_EQ(key1, cmdline_argv[i])) {
                        g_free(key1);
                        return i;
                }
        }

        /* look for second key if one was specified */
        if (key2) {
                for (int i = 0; i < cmdline_argc; i++) {
                        if (STR_EQ(key2, cmdline_argv[i])) {
                                g_free(key1);
                                return i;
                        }
                }
        }

        g_free(key1);
        return -1;
}

static const char *cmdline_get_value(const char *key)
{
        int idx = cmdline_find_option(key);
        if (idx < 0) {
                return NULL;
        }

        if (idx + 1 >= cmdline_argc) {
                /* the argument is missing */
                LOG_W("%s: Missing argument. Ignoring.", key);
                return NULL;
        }
        return cmdline_argv[idx + 1];
}

char *cmdline_get_string(const char *key, const char *def, const char *description)
{
        cmdline_usage_append(key, "string", description);
        const char *str = cmdline_get_value(key);

        if (str)
                return g_strdup(str);
        if (def)
                return g_strdup(def);
        else
                return NULL;
}

char *cmdline_get_path(const char *key, const char *def, const char *description)
{
        cmdline_usage_append(key, "string", description);
        const char *str = cmdline_get_value(key);

        if (str)
                return string_to_path(g_strdup(str));
        else
                return string_to_path(g_strdup(def));
}

char **cmdline_get_list(const char *key, const char *def, const char *description)
{
        cmdline_usage_append(key, "list", description);
        const char *str = cmdline_get_value(key);

        if (str)
                return string_to_array(str);
        else
                return string_to_array(def);
}

gint64 cmdline_get_time(const char *key, gint64 def, const char *description)
{
        cmdline_usage_append(key, "time", description);
        const char *timestring = cmdline_get_value(key);
        gint64 val = def;

        if (timestring) {
                val = string_to_time(timestring);
        }

        return val;
}

int cmdline_get_int(const char *key, int def, const char *description)
{
        cmdline_usage_append(key, "int", description);
        const char *str = cmdline_get_value(key);

        if (str)
                return atoi(str);
        else
                return def;
}

double cmdline_get_double(const char *key, double def, const char *description)
{
        cmdline_usage_append(key, "double", description);
        const char *str = cmdline_get_value(key);

        if (str)
                return atof(str);
        else
                return def;
}

int cmdline_get_bool(const char *key, int def, const char *description)
{
        cmdline_usage_append(key, "", description);
        int idx = cmdline_find_option(key);

        if (idx > 0)
                return true;
        else
                return def;
}

bool cmdline_is_set(const char *key)
{
        return cmdline_get_value(key) != NULL;
}

void cmdline_usage_append(const char *key, const char *type, const char *description)
{
        char *key_type;
        if (STR_FULL(type))
                key_type = g_strdup_printf("%s (%s)", key, type);
        else
                key_type = g_strdup(key);

        if (!usage_str) {
                usage_str =
                    g_strdup_printf("%-40s - %s\n", key_type, description);
                g_free(key_type);
                return;
        }

        char *tmp;
        tmp =
            g_strdup_printf("%s%-40s - %s\n", usage_str, key_type, description);
        g_free(key_type);

        g_free(usage_str);
        usage_str = tmp;

}

const char *cmdline_create_usage(void)
{
        return usage_str;
}

/* vim: set ft=c tabstop=8 shiftwidth=8 expandtab textwidth=0: */
