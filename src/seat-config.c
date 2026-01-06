/*
 * Shared helpers to load seat configuration sections.
 */

#include "seat-config.h"

#include "configuration.h"
#include "logger.h"

GList *
get_config_sections (const gchar *seat_name)
{
    /* Load seat defaults first */
    GList *config_sections = g_list_append (NULL, g_strdup ("Seat:*"));

    g_auto(GStrv) groups = config_get_groups (config_get_instance ());
    for (gchar **i = groups; *i; i++)
    {
        if (g_str_has_prefix (*i, "Seat:") && strcmp (*i, "Seat:*") != 0)
        {
            const gchar *seat_name_glob = *i + strlen ("Seat:");
            if (g_pattern_match_simple (seat_name_glob, seat_name ? seat_name : ""))
                config_sections = g_list_append (config_sections, g_strdup (*i));
        }
    }

    return config_sections;
}

void
seat_config_apply (Seat *seat, const gchar *seat_name)
{
    GList *sections = get_config_sections (seat_name);
    for (GList *link = sections; link; link = link->next)
    {
        const gchar *section = link->data;
        g_auto(GStrv) keys = NULL;

        keys = config_get_keys (config_get_instance (), section);

        l_debug (seat, "Loading properties from config section %s", section);
        for (gint i = 0; keys && keys[i]; i++)
        {
            g_autofree gchar *value = config_get_string (config_get_instance (), section, keys[i]);
            seat_set_property (seat, keys[i], value);
        }
    }
    g_list_free_full (sections, g_free);
}
