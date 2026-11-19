/* Prints what parse_feed finds in each feed file given on the command line. */
#include "../feeds.c"

int main(int argc, char **argv)
{
    int i;

    for (i = 1; i < argc; i++) {
        gchar *text = NULL, *title = NULL;
        gsize len = 0;
        GList *items, *l;
        guint dated = 0;

        if (!g_file_get_contents(argv[i], &text, &len, NULL)) {
            printf("%s: unreadable\n", argv[i]);
            continue;
        }
        items = parse_feed(text, len, &title);
        for (l = items; l; l = l->next)
            if (((FeedItem *)l->data)->when)
                dated++;
        items = g_list_sort(items, compare_items);
        printf("%s: \"%s\", %u items, %u dated\n", argv[i], title ? title : "(none)",
               g_list_length(items), dated);
        if (items) {
            FeedItem *newest = g_list_last(items)->data;
            gchar *when = g_date_time_format(g_date_time_new_from_unix_local(newest->when), "%F %R");
            printf("  newest: %s | %s\n  link: %s\n  id: %s\n", when, newest->title,
                   newest->link ? newest->link : "(none)", newest->id);
        }
        g_list_free_full(items, (GDestroyNotify)feed_item_free);
        g_free(title);
        g_free(text);
    }
    return 0;
}
