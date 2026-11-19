/* Prints what parse_feed finds in each feed file, in delivery order, with
 * times in UTC so the output can be compared against tests/expected.txt. */
#include "../libfeeds.c"

int main(int argc, char **argv)
{
    int i;

    for (i = 1; i < argc; i++) {
        gchar *text = NULL, *title = NULL, *name = g_path_get_basename(argv[i]);
        gsize len = 0;
        GList *items, *l;

        if (!g_file_get_contents(argv[i], &text, &len, NULL)) {
            printf("%s: unreadable\n", name);
            return 1;
        }
        items = g_list_sort(parse_feed(text, len, &title), compare_items);
        printf("%s: \"%s\", %u items\n", name, title ? title : "(none)", g_list_length(items));
        for (l = items; l; l = l->next) {
            FeedItem *item = l->data;
            GDateTime *dt = item->when ? g_date_time_new_from_unix_utc(item->when) : NULL;
            gchar *when = dt ? g_date_time_format(dt, "%FT%TZ") : g_strdup("(no date)");
            printf("  %s | %s | %s | %s\n", when, item->title,
                   item->link ? item->link : "(no link)", item->id);
            g_free(when);
            if (dt)
                g_date_time_unref(dt);
        }
        g_list_free_full(items, (GDestroyNotify)feed_item_free);
        g_free(title);
        g_free(text);
        g_free(name);
    }
    return 0;
}
