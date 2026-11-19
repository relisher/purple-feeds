/*
 * purple-feeds: RSS and Atom feeds for Pidgin and other libpurple clients.
 * Each Feeds account has one buddy, "news", and the feeds configured on the
 * account; new articles from every feed arrive as messages from that buddy.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#define PURPLE_PLUGINS

#include <errno.h>
#include <string.h>

#include <glib.h>
#include <glib/gstdio.h>

#include "account.h"
#include "accountopt.h"
#include "blist.h"
#include "connection.h"
#include "debug.h"
#include "plugin.h"
#include "prpl.h"
#include "request.h"
#include "server.h"
#include "util.h"
#include "version.h"
#include "xmlnode.h"

#define FEEDS_PLUGIN_ID "prpl-relisher-feeds"
#define FEEDS_VERSION "0.2.0"
#define FEEDS_USER_AGENT "Mozilla/5.0 (compatible; purple-feeds/" FEEDS_VERSION ")"

#define NEWS_BUDDY "news"
#define NEWS_GROUP "News"

#define OPT_INTERVAL "check_interval"
#define OPT_INITIAL_ITEMS "initial_items"
/* One feed per line: URL, name and link filter separated by tabs. */
#define SETTING_FEEDS "feeds"

#define DEFAULT_INTERVAL_MINUTES 10
#define DEFAULT_INITIAL_ITEMS 5
#define MAX_FEED_BYTES (8 * 1024 * 1024)
/* Enough to remember every item a feed still lists, plus some it dropped. */
#define MAX_SEEN_IDS 1000

typedef struct {
    gchar *url;
    gchar *name;    /* NULL until given or learned from the feed's title */
    gchar *filter;  /* regex the article link must match, or NULL */
    gchar *error;   /* last fetch error, or NULL */
} Feed;

typedef struct {
    PurpleConnection *gc;
    guint timer;
    GSList *fetches;
    GList *feeds;
} FeedsConnection;

typedef struct {
    FeedsConnection *fc;
    gchar *url;
    PurpleUtilFetchUrlData *request;
} FeedFetch;

typedef struct {
    gchar *id;
    gchar *title;
    gchar *link;
    time_t when;
    guint order;
} FeedItem;

/* ------------------------------------------------------------------------ */
/* Dates                                                                    */

static const gchar *MONTHS[] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};

/* Offset in seconds for the zone names RFC 822 allows. */
static gint zone_offset(const gchar *zone)
{
    static const struct { const gchar *name; gint hours; } names[] = {
        {"UT", 0}, {"UTC", 0}, {"GMT", 0}, {"Z", 0},
        {"EST", -5}, {"EDT", -4}, {"CST", -6}, {"CDT", -5},
        {"MST", -7}, {"MDT", -6}, {"PST", -8}, {"PDT", -7},
    };
    guint i;

    if ((zone[0] == '+' || zone[0] == '-') && strlen(zone) >= 5) {
        gint hhmm = atoi(zone + 1);
        gint seconds = (hhmm / 100) * 3600 + (hhmm % 100) * 60;
        return zone[0] == '-' ? -seconds : seconds;
    }
    for (i = 0; i < G_N_ELEMENTS(names); i++)
        if (g_ascii_strcasecmp(zone, names[i].name) == 0)
            return names[i].hours * 3600;
    return 0;
}

/* Parse "Thu, 19 Nov 2026 15:04:05 GMT" (the weekday is optional). */
static time_t parse_rfc822(const gchar *text)
{
    gchar month[4] = "", zone[8] = "";
    guint day, year, hour, minute, second = 0;
    const gchar *comma = strchr(text, ',');
    GDateTime *utc, *dt;
    time_t result;
    gint m;

    if (comma)
        text = comma + 1;
    if (sscanf(text, " %u %3s %u %u:%u:%u %7s", &day, month, &year, &hour,
               &minute, &second, zone) < 5)
        return 0;
    for (m = 0; m < 12; m++)
        if (g_ascii_strcasecmp(month, MONTHS[m]) == 0)
            break;
    if (m == 12)
        return 0;
    if (year < 100)
        year += year < 70 ? 2000 : 1900;

    utc = g_date_time_new_utc(year, m + 1, day, hour, minute, second);
    if (!utc)
        return 0;
    dt = g_date_time_add_seconds(utc, -zone_offset(zone));
    result = g_date_time_to_unix(dt);
    g_date_time_unref(dt);
    g_date_time_unref(utc);
    return result;
}

static time_t parse_iso8601(const gchar *text)
{
    GDateTime *dt = g_date_time_new_from_iso8601(text, NULL);
    time_t result;

    if (!dt)
        return 0;
    result = g_date_time_to_unix(dt);
    g_date_time_unref(dt);
    return result;
}

/* ------------------------------------------------------------------------ */
/* Parsing                                                                  */

static gchar *child_text(xmlnode *parent, const gchar *name)
{
    xmlnode *child = xmlnode_get_child(parent, name);
    gchar *text = child ? xmlnode_get_data_unescaped(child) : NULL;

    if (text)
        g_strstrip(text);
    if (text && *text == '\0') {
        g_free(text);
        text = NULL;
    }
    return text;
}

static gchar *atom_link(xmlnode *entry)
{
    xmlnode *link;

    for (link = xmlnode_get_child(entry, "link"); link;
         link = xmlnode_get_next_twin(link)) {
        const gchar *rel = xmlnode_get_attrib(link, "rel");
        const gchar *href = xmlnode_get_attrib(link, "href");
        if (href && (!rel || g_str_equal(rel, "alternate")))
            return g_strdup(href);
    }
    return NULL;
}

static void feed_item_free(FeedItem *item)
{
    g_free(item->id);
    g_free(item->title);
    g_free(item->link);
    g_free(item);
}

static FeedItem *feed_item_new(gchar *id, gchar *title, gchar *link,
                               time_t when, guint order)
{
    FeedItem *item = g_new0(FeedItem, 1);

    item->title = title ? title : g_strdup(link ? link : "(untitled)");
    item->link = link;
    item->id = id ? id : g_strdup(link ? link : item->title);
    item->when = when;
    item->order = order;
    return item;
}

/* Return the items of an RSS 2.0, RSS 1.0 or Atom document, and the feed's
 * own title in *feed_title. */
static GList *parse_feed(const gchar *text, gsize len, gchar **feed_title)
{
    xmlnode *root = xmlnode_from_str(text, len);
    xmlnode *channel, *node;
    GList *items = NULL;
    guint order = 0;

    *feed_title = NULL;
    if (!root)
        return NULL;

    if (g_str_equal(root->name, "feed")) {
        *feed_title = child_text(root, "title");
        for (node = xmlnode_get_child(root, "entry"); node;
             node = xmlnode_get_next_twin(node)) {
            gchar *date = child_text(node, "published");
            if (!date)
                date = child_text(node, "updated");
            items = g_list_prepend(items, feed_item_new(
                    child_text(node, "id"), child_text(node, "title"),
                    atom_link(node), date ? parse_iso8601(date) : 0, order++));
            g_free(date);
        }
    } else {
        channel = xmlnode_get_child(root, "channel");
        if (channel)
            *feed_title = child_text(channel, "title");
        /* RSS 2.0 nests items in the channel; RSS 1.0 puts them at the top. */
        node = channel ? xmlnode_get_child(channel, "item") : NULL;
        if (!node)
            node = xmlnode_get_child(root, "item");
        for (; node; node = xmlnode_get_next_twin(node)) {
            gchar *date = child_text(node, "pubDate");
            time_t when = 0;
            if (date)
                when = parse_rfc822(date);
            else if ((date = child_text(node, "date")) != NULL)
                when = parse_iso8601(date);
            items = g_list_prepend(items, feed_item_new(
                    child_text(node, "guid"), child_text(node, "title"),
                    child_text(node, "link"), when, order++));
            g_free(date);
        }
    }

    xmlnode_free(root);
    return g_list_reverse(items);
}

/* Oldest first; undated items keep the feed's newest-first order reversed. */
static gint compare_items(gconstpointer a, gconstpointer b)
{
    const FeedItem *x = a, *y = b;

    if (x->when && y->when && x->when != y->when)
        return x->when < y->when ? -1 : 1;
    return x->order > y->order ? -1 : (x->order < y->order ? 1 : 0);
}

/* ------------------------------------------------------------------------ */
/* Seen items, kept per feed in ~/.purple/feeds/                            */

static gchar *seen_path(PurpleAccount *account, const gchar *url)
{
    gchar *key = g_strdup_printf("%s\n%s", purple_account_get_username(account), url);
    gchar *hash = g_compute_checksum_for_string(G_CHECKSUM_SHA1, key, -1);
    gchar *path = g_build_filename(purple_user_dir(), "feeds", hash, NULL);

    g_free(hash);
    g_free(key);
    return path;
}

/* NULL when the feed has never been fetched. */
static GHashTable *seen_load(PurpleAccount *account, const gchar *url)
{
    gchar *path = seen_path(account, url);
    gchar *contents = NULL;
    GHashTable *seen = NULL;

    if (g_file_get_contents(path, &contents, NULL, NULL)) {
        gchar **ids = g_strsplit(contents, "\n", -1), **id;
        seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
        for (id = ids; *id; id++)
            if (**id)
                g_hash_table_add(seen, g_strdup(*id));
        g_strfreev(ids);
        g_free(contents);
    }
    g_free(path);
    return seen;
}

/* Store the feed's current ids first, then older ones up to the cap. */
static void seen_save(PurpleAccount *account, const gchar *url, GList *items,
                      GHashTable *old)
{
    gchar *path = seen_path(account, url);
    gchar *dir = g_path_get_dirname(path);
    GHashTable *written = g_hash_table_new(g_str_hash, g_str_equal);
    GString *out = g_string_new(NULL);
    GHashTableIter iter;
    gpointer id;
    GList *l;

    for (l = items; l; l = l->next) {
        FeedItem *item = l->data;
        if (g_hash_table_size(written) < MAX_SEEN_IDS &&
            !g_hash_table_contains(written, item->id) &&
            !strchr(item->id, '\n')) {
            g_hash_table_add(written, item->id);
            g_string_append_printf(out, "%s\n", item->id);
        }
    }
    if (old) {
        g_hash_table_iter_init(&iter, old);
        while (g_hash_table_iter_next(&iter, &id, NULL) &&
               g_hash_table_size(written) < MAX_SEEN_IDS) {
            if (!g_hash_table_contains(written, id)) {
                g_hash_table_add(written, id);
                g_string_append_printf(out, "%s\n", (gchar *)id);
            }
        }
    }

    g_mkdir_with_parents(dir, 0700);
    if (!g_file_set_contents(path, out->str, out->len, NULL))
        purple_debug_error("feeds", "could not write %s\n", path);

    g_string_free(out, TRUE);
    g_hash_table_destroy(written);
    g_free(dir);
    g_free(path);
}


/* ------------------------------------------------------------------------ */
/* The feed list, stored on the account                                     */

static void feed_free(Feed *feed)
{
    g_free(feed->url);
    g_free(feed->name);
    g_free(feed->filter);
    g_free(feed->error);
    g_free(feed);
}

/* Tabs and newlines separate the stored fields, so they cannot appear in one. */
static gchar *clean_field(const gchar *text)
{
    gchar *clean;

    if (!text)
        return NULL;
    clean = g_strstrip(g_strdelimit(g_strdup(text), "\t\r\n", ' '));
    if (*clean == '\0') {
        g_free(clean);
        return NULL;
    }
    return clean;
}

static GList *feeds_load(PurpleAccount *account)
{
    const gchar *stored = purple_account_get_string(account, SETTING_FEEDS, "");
    gchar **lines = g_strsplit(stored, "\n", -1), **line;
    GList *feeds = NULL;

    for (line = lines; *line; line++) {
        gchar **fields = g_strsplit(*line, "\t", 3);
        if (fields[0] && *fields[0]) {
            Feed *feed = g_new0(Feed, 1);
            feed->url = g_strdup(fields[0]);
            feed->name = fields[1] ? clean_field(fields[1]) : NULL;
            feed->filter = fields[1] && fields[2] ? clean_field(fields[2]) : NULL;
            feeds = g_list_append(feeds, feed);
        }
        g_strfreev(fields);
    }
    g_strfreev(lines);
    return feeds;
}

static void feeds_save(PurpleAccount *account, GList *feeds)
{
    GString *out = g_string_new(NULL);
    GList *l;

    for (l = feeds; l; l = l->next) {
        Feed *feed = l->data;
        g_string_append_printf(out, "%s\t%s\t%s\n", feed->url,
                               feed->name ? feed->name : "",
                               feed->filter ? feed->filter : "");
    }
    purple_account_set_string(account, SETTING_FEEDS, out->str);
    g_string_free(out, TRUE);
}

static const gchar *feed_label(const Feed *feed)
{
    return feed->name ? feed->name : feed->url;
}

/* Find a feed by URL, or by name ignoring case. */
static Feed *feed_find(FeedsConnection *fc, const gchar *key)
{
    gchar *folded = g_utf8_casefold(key, -1);
    GList *l;

    for (l = fc->feeds; l; l = l->next) {
        Feed *feed = l->data;
        gchar *name = feed->name ? g_utf8_casefold(feed->name, -1) : NULL;
        gboolean match = g_str_equal(feed->url, key) ||
                         (name && g_str_equal(name, folded));
        g_free(name);
        if (match) {
            g_free(folded);
            return feed;
        }
    }
    g_free(folded);
    return NULL;
}

/* ------------------------------------------------------------------------ */
/* The news buddy                                                           */

static PurpleBuddy *news_buddy(PurpleAccount *account)
{
    PurpleBuddy *buddy = purple_find_buddy(account, NEWS_BUDDY);
    PurpleGroup *group;

    if (buddy)
        return buddy;
    group = purple_find_group(NEWS_GROUP);
    if (!group) {
        group = purple_group_new(NEWS_GROUP);
        purple_blist_add_group(group, NULL);
    }
    buddy = purple_buddy_new(account, NEWS_BUDDY, "News");
    purple_blist_add_buddy(buddy, NULL, group, NULL);
    return buddy;
}

static void update_status(FeedsConnection *fc)
{
    purple_prpl_got_user_status(purple_connection_get_account(fc->gc),
                                NEWS_BUDDY, "available", NULL);
}

typedef struct {
    PurpleConnection *gc;
    gchar *html;
} Reply;

static gboolean reply_cb(gpointer data)
{
    Reply *reply = data;

    if (PURPLE_CONNECTION_IS_VALID(reply->gc))
        serv_got_im(reply->gc, NEWS_BUDDY, reply->html, PURPLE_MESSAGE_RECV, time(NULL));
    g_free(reply->html);
    g_free(reply);
    return FALSE;
}

/* Replies to a command show after the command itself, so defer them. */
static void reply(FeedsConnection *fc, gchar *html)
{
    Reply *r = g_new0(Reply, 1);

    r->gc = fc->gc;
    r->html = html;
    purple_timeout_add(0, reply_cb, r);
}

/* ------------------------------------------------------------------------ */
/* Fetching and delivery                                                    */

static void feeds_fetch(FeedsConnection *fc, Feed *feed);

static gboolean link_allowed(const Feed *feed, const FeedItem *item)
{
    GRegex *regex;
    gboolean allowed;

    if (!feed->filter)
        return TRUE;
    regex = g_regex_new(feed->filter, G_REGEX_CASELESS, 0, NULL);
    if (!regex)
        return TRUE;
    allowed = item->link && g_regex_match(regex, item->link, 0, NULL);
    g_regex_unref(regex);
    return allowed;
}

static void deliver(FeedsConnection *fc, const Feed *feed, const FeedItem *item)
{
    gchar *source = g_markup_escape_text(feed_label(feed), -1);
    gchar *title = g_markup_escape_text(item->title, -1);
    gchar *message;

    if (item->link) {
        gchar *href = g_markup_escape_text(item->link, -1);
        message = g_strdup_printf("<b>%s:</b> <a href=\"%s\">%s</a>", source, href, title);
        g_free(href);
    } else {
        message = g_strdup_printf("<b>%s:</b> %s", source, title);
    }
    serv_got_im(fc->gc, NEWS_BUDDY, message, PURPLE_MESSAGE_RECV,
                item->when ? item->when : time(NULL));
    g_free(message);
    g_free(title);
    g_free(source);
}

static void set_feed_error(FeedsConnection *fc, Feed *feed, const gchar *error)
{
    g_free(feed->error);
    feed->error = g_strdup(error);
    update_status(fc);
}

static void handle_feed(FeedsConnection *fc, Feed *feed, const gchar *text, gsize len)
{
    PurpleAccount *account = purple_connection_get_account(fc->gc);
    gchar *feed_title = NULL;
    GList *items = parse_feed(text, len, &feed_title);
    GHashTable *seen = seen_load(account, feed->url);
    GList *fresh = NULL, *l;
    gint limit = purple_account_get_int(account, OPT_INITIAL_ITEMS,
                                        DEFAULT_INITIAL_ITEMS);

    if (!items && !feed_title) {
        set_feed_error(fc, feed, "Not an RSS or Atom feed");
        if (seen)
            g_hash_table_destroy(seen);
        return;
    }

    if (!feed->name && feed_title) {
        feed->name = clean_field(feed_title);
        feeds_save(account, fc->feeds);
    }

    for (l = items; l; l = l->next) {
        FeedItem *item = l->data;
        if (seen && g_hash_table_contains(seen, item->id))
            continue;
        if (!link_allowed(feed, item))
            continue;
        fresh = g_list_prepend(fresh, item);
    }
    fresh = g_list_sort(fresh, compare_items);

    /* On the first fetch, show only the newest few items. */
    if (!seen) {
        gint skip = (gint)g_list_length(fresh) - limit;
        while (skip-- > 0)
            fresh = g_list_delete_link(fresh, fresh);
    }

    for (l = fresh; l; l = l->next)
        deliver(fc, feed, l->data);
    purple_debug_info("feeds", "%s: %u items, %u new\n", feed->url,
                      g_list_length(items), g_list_length(fresh));

    seen_save(account, feed->url, items, seen);
    set_feed_error(fc, feed, NULL);

    g_list_free(fresh);
    g_list_free_full(items, (GDestroyNotify)feed_item_free);
    if (seen)
        g_hash_table_destroy(seen);
    g_free(feed_title);
}

static void fetch_done(PurpleUtilFetchUrlData *request, gpointer data,
                       const gchar *text, gsize len, const gchar *error)
{
    FeedFetch *fetch = data;
    FeedsConnection *fc = fetch->fc;
    Feed *feed = feed_find(fc, fetch->url);

    fc->fetches = g_slist_remove(fc->fetches, fetch);

    /* The feed may have been removed while the fetch was running. */
    if (feed) {
        if (error) {
            purple_debug_warning("feeds", "%s: %s\n", fetch->url, error);
            set_feed_error(fc, feed, error);
        } else {
            handle_feed(fc, feed, text, len);
        }
    }

    g_free(fetch->url);
    g_free(fetch);
}

static void feeds_fetch(FeedsConnection *fc, Feed *feed)
{
    FeedFetch *fetch;
    GSList *l;

    for (l = fc->fetches; l; l = l->next)
        if (g_str_equal(((FeedFetch *)l->data)->url, feed->url))
            return;

    fetch = g_new0(FeedFetch, 1);
    fetch->fc = fc;
    fetch->url = g_strdup(feed->url);
    fetch->request = purple_util_fetch_url_request_len_with_account(
            purple_connection_get_account(fc->gc), feed->url, TRUE,
            FEEDS_USER_AGENT, TRUE, NULL, FALSE, MAX_FEED_BYTES, fetch_done, fetch);
    if (fetch->request) {
        fc->fetches = g_slist_prepend(fc->fetches, fetch);
    } else {
        g_free(fetch->url);
        g_free(fetch);
    }
}

static void feeds_fetch_all(FeedsConnection *fc)
{
    GList *l;

    for (l = fc->feeds; l; l = l->next)
        feeds_fetch(fc, l->data);
}

static gboolean poll_cb(gpointer data)
{
    feeds_fetch_all(data);
    return TRUE;
}

/* ------------------------------------------------------------------------ */
/* Adding and removing feeds                                                */

static gboolean valid_filter(const gchar *pattern, gchar **error_out)
{
    GError *error = NULL;
    GRegex *regex;

    if (!pattern)
        return TRUE;
    regex = g_regex_new(pattern, 0, 0, &error);
    if (!regex) {
        *error_out = g_strdup(error->message);
        g_error_free(error);
        return FALSE;
    }
    g_regex_unref(regex);
    return TRUE;
}

/* Returns NULL on success, or an error message to show. */
static gchar *add_feed(FeedsConnection *fc, const gchar *url, const gchar *name,
                       const gchar *filter)
{
    gchar *clean_url = clean_field(url);
    gchar *error = NULL;
    Feed *feed;

    if (!clean_url || (!g_str_has_prefix(clean_url, "http://") &&
                       !g_str_has_prefix(clean_url, "https://"))) {
        g_free(clean_url);
        return g_strdup("The feed address must start with http:// or https://");
    }
    if (feed_find(fc, clean_url)) {
        g_free(clean_url);
        return g_strdup("That feed is already in the list");
    }
    feed = g_new0(Feed, 1);
    feed->url = clean_url;
    feed->name = clean_field(name);
    feed->filter = clean_field(filter);
    if (!valid_filter(feed->filter, &error)) {
        gchar *message = g_strdup_printf("Invalid link filter: %s", error);
        g_free(error);
        feed_free(feed);
        return message;
    }

    fc->feeds = g_list_append(fc->feeds, feed);
    feeds_save(purple_connection_get_account(fc->gc), fc->feeds);
    feeds_fetch(fc, feed);
    return NULL;
}

static void remove_feed(FeedsConnection *fc, Feed *feed)
{
    gchar *path = seen_path(purple_connection_get_account(fc->gc), feed->url);

    g_unlink(path);
    g_free(path);
    fc->feeds = g_list_remove(fc->feeds, feed);
    feed_free(feed);
    feeds_save(purple_connection_get_account(fc->gc), fc->feeds);
    update_status(fc);
}

static FeedsConnection *connection_of(PurpleAccount *account)
{
    PurpleConnection *gc = purple_account_get_connection(account);

    return gc ? purple_connection_get_protocol_data(gc) : NULL;
}

static void add_dialog_ok(PurpleAccount *account, PurpleRequestFields *fields)
{
    FeedsConnection *fc = connection_of(account);
    gchar *error;

    if (!fc)
        return;
    error = add_feed(fc, purple_request_fields_get_string(fields, "url"),
                     purple_request_fields_get_string(fields, "name"),
                     purple_request_fields_get_string(fields, "filter"));
    if (error) {
        purple_notify_error(fc->gc, "Feeds", "Could not add the feed", error);
        g_free(error);
    }
}

static void show_add_dialog(PurpleAccount *account)
{
    PurpleRequestFields *fields = purple_request_fields_new();
    PurpleRequestFieldGroup *group = purple_request_field_group_new(NULL);
    PurpleRequestField *field;

    field = purple_request_field_string_new("url", "Feed address", NULL, FALSE);
    purple_request_field_set_required(field, TRUE);
    purple_request_field_group_add_field(group, field);
    purple_request_field_group_add_field(group,
            purple_request_field_string_new("name", "Name (optional)", NULL, FALSE));
    purple_request_field_group_add_field(group,
            purple_request_field_string_new("filter", "Link filter (optional regex)", NULL, FALSE));
    purple_request_fields_add_group(fields, group);

    purple_request_fields(account, "Add feed", "Add an RSS or Atom feed",
            "Without a name, the feed's own title is used.", fields,
            "Add", G_CALLBACK(add_dialog_ok), "Cancel", NULL,
            account, NEWS_BUDDY, NULL, account);
}

static void remove_dialog_ok(PurpleAccount *account, PurpleRequestFields *fields)
{
    FeedsConnection *fc = connection_of(account);
    PurpleRequestField *field = purple_request_fields_get_field(fields, "feeds");
    GList *selected, *l;

    if (!fc || !field)
        return;
    selected = g_list_copy(purple_request_field_list_get_selected(field));
    for (l = selected; l; l = l->next) {
        Feed *feed = feed_find(fc, purple_request_field_list_get_data(field, l->data));
        if (feed)
            remove_feed(fc, feed);
    }
    g_list_free(selected);
}

static void show_remove_dialog(PurpleAccount *account)
{
    FeedsConnection *fc = connection_of(account);
    PurpleRequestFields *fields;
    PurpleRequestFieldGroup *group;
    PurpleRequestField *field;
    GList *l;

    if (!fc)
        return;
    if (!fc->feeds) {
        purple_notify_info(fc->gc, "Feeds", "There are no feeds to remove.", NULL);
        return;
    }
    fields = purple_request_fields_new();
    group = purple_request_field_group_new(NULL);
    field = purple_request_field_list_new("feeds", "Feeds");
    purple_request_field_list_set_multi_select(field, TRUE);
    for (l = fc->feeds; l; l = l->next) {
        Feed *feed = l->data;
        purple_request_field_list_add_icon(field, feed_label(feed), NULL, feed->url);
    }
    purple_request_field_group_add_field(group, field);
    purple_request_fields_add_group(fields, group);

    purple_request_fields(account, "Remove feeds", "Select the feeds to remove",
            NULL, fields, "Remove", G_CALLBACK(remove_dialog_ok), "Cancel", NULL,
            account, NEWS_BUDDY, NULL, account);
}

/* ------------------------------------------------------------------------ */
/* Commands sent as messages to the news buddy                              */

static gchar *feed_list_html(FeedsConnection *fc)
{
    GString *out = g_string_new(NULL);
    GList *l;

    if (!fc->feeds)
        return g_strdup("No feeds yet. Send <b>add &lt;url&gt;</b> to add one.");
    g_string_append(out, "Feeds:");
    for (l = fc->feeds; l; l = l->next) {
        Feed *feed = l->data;
        gchar *label = g_markup_escape_text(feed_label(feed), -1);
        gchar *url = g_markup_escape_text(feed->url, -1);
        g_string_append_printf(out, "<br>• <b>%s</b> — %s", label, url);
        if (feed->filter) {
            gchar *filter = g_markup_escape_text(feed->filter, -1);
            g_string_append_printf(out, " (links matching <i>%s</i>)", filter);
            g_free(filter);
        }
        if (feed->error) {
            gchar *error = g_markup_escape_text(feed->error, -1);
            g_string_append_printf(out, " — last check failed: %s", error);
            g_free(error);
        }
        g_free(url);
        g_free(label);
    }
    return g_string_free(out, FALSE);
}

static const gchar *HELP_HTML =
    "Commands:"
    "<br><b>add &lt;url&gt; [name]</b> adds a feed"
    "<br><b>remove &lt;name or url&gt;</b> removes one"
    "<br><b>filter &lt;name or url&gt; [regex]</b> shows only articles whose link"
    " matches; no regex clears it"
    "<br><b>list</b> shows the feeds"
    "<br><b>check</b> checks every feed now";

static void run_command(FeedsConnection *fc, const gchar *text)
{
    gchar *plain = purple_markup_strip_html(text);
    gchar **words = g_strsplit(g_strstrip(plain), " ", 2);
    const gchar *command = words[0] ? words[0] : "";
    gchar *arg = words[0] && words[1] ? g_strstrip(g_strdup(words[1])) : g_strdup("");
    gchar *error = NULL;

    if (g_ascii_strcasecmp(command, "add") == 0 && *arg) {
        gchar **parts = g_strsplit(arg, " ", 2);
        error = add_feed(fc, parts[0], parts[1], NULL);
        if (!error)
            reply(fc, g_strdup("Added. Its newest articles will appear shortly."));
        g_strfreev(parts);
    } else if (g_ascii_strcasecmp(command, "remove") == 0 && *arg) {
        Feed *feed = feed_find(fc, arg);
        if (feed) {
            remove_feed(fc, feed);
            reply(fc, g_strdup("Removed."));
        } else {
            error = g_strdup("No feed with that name or address. Send list to see them.");
        }
    } else if (g_ascii_strcasecmp(command, "filter") == 0 && *arg) {
        GList *l;
        Feed *feed = NULL;
        const gchar *pattern = NULL;
        /* Feed names may contain spaces, so try the longest matching prefix. */
        for (l = fc->feeds; l && !feed; l = l->next) {
            Feed *f = l->data;
            const gchar *keys[] = { f->url, f->name };
            guint k;
            for (k = 0; k < G_N_ELEMENTS(keys) && !feed; k++) {
                gsize n = keys[k] ? strlen(keys[k]) : 0;
                if (n && g_ascii_strncasecmp(arg, keys[k], n) == 0 &&
                    (arg[n] == '\0' || arg[n] == ' ')) {
                    feed = f;
                    pattern = arg + n;
                }
            }
        }
        if (!feed) {
            error = g_strdup("No feed with that name or address. Send list to see them.");
        } else {
            gchar *clean = clean_field(pattern);
            if (valid_filter(clean, &error)) {
                g_free(feed->filter);
                feed->filter = clean;
                feeds_save(purple_connection_get_account(fc->gc), fc->feeds);
                reply(fc, g_strdup(clean ? "Filter set." : "Filter cleared."));
            } else {
                g_free(clean);
            }
        }
    } else if (g_ascii_strcasecmp(command, "list") == 0) {
        reply(fc, feed_list_html(fc));
    } else if (g_ascii_strcasecmp(command, "check") == 0) {
        feeds_fetch_all(fc);
        reply(fc, g_strdup("Checking every feed."));
    } else {
        reply(fc, g_strdup(HELP_HTML));
    }

    if (error) {
        gchar *escaped = g_markup_escape_text(error, -1);
        reply(fc, escaped);
        g_free(error);
    }
    g_free(arg);
    g_strfreev(words);
    g_free(plain);
}

/* ------------------------------------------------------------------------ */
/* Protocol callbacks                                                       */

static const char *feeds_list_icon(PurpleAccount *account, PurpleBuddy *buddy)
{
    return "feeds";
}

static GList *feeds_status_types(PurpleAccount *account)
{
    GList *types = NULL;

    types = g_list_append(types, purple_status_type_new_full(
            PURPLE_STATUS_AVAILABLE, "available", NULL, TRUE, TRUE, FALSE));
    types = g_list_append(types, purple_status_type_new_full(
            PURPLE_STATUS_OFFLINE, "offline", NULL, TRUE, TRUE, FALSE));
    return types;
}

static guint failing_feeds(FeedsConnection *fc)
{
    guint failing = 0;
    GList *l;

    for (l = fc->feeds; l; l = l->next)
        if (((Feed *)l->data)->error)
            failing++;
    return failing;
}

static char *feeds_status_text(PurpleBuddy *buddy)
{
    FeedsConnection *fc = connection_of(purple_buddy_get_account(buddy));
    guint failing;

    if (!fc)
        return NULL;
    failing = failing_feeds(fc);
    if (failing)
        return g_strdup_printf("%u of %u feeds failing", failing, g_list_length(fc->feeds));
    return g_strdup_printf("%u feeds", g_list_length(fc->feeds));
}

static void feeds_tooltip_text(PurpleBuddy *buddy, PurpleNotifyUserInfo *info,
                               gboolean full)
{
    FeedsConnection *fc = connection_of(purple_buddy_get_account(buddy));
    GList *l;

    if (!fc)
        return;
    for (l = fc->feeds; l; l = l->next) {
        Feed *feed = l->data;
        purple_notify_user_info_add_pair_plaintext(info, feed_label(feed),
                feed->error ? feed->error : "OK");
    }
}

static void feeds_login(PurpleAccount *account)
{
    PurpleConnection *gc = purple_account_get_connection(account);
    FeedsConnection *fc = g_new0(FeedsConnection, 1);
    gint minutes = purple_account_get_int(account, OPT_INTERVAL,
                                          DEFAULT_INTERVAL_MINUTES);

    fc->gc = gc;
    fc->feeds = feeds_load(account);
    purple_connection_set_protocol_data(gc, fc);
    news_buddy(account);
    purple_connection_set_state(gc, PURPLE_CONNECTED);
    update_status(fc);
    fc->timer = purple_timeout_add_seconds(MAX(minutes, 1) * 60, poll_cb, fc);
    feeds_fetch_all(fc);
}

static void feeds_close(PurpleConnection *gc)
{
    FeedsConnection *fc = purple_connection_get_protocol_data(gc);

    if (!fc)
        return;
    if (fc->timer)
        purple_timeout_remove(fc->timer);
    while (fc->fetches) {
        FeedFetch *fetch = fc->fetches->data;
        purple_util_fetch_url_cancel(fetch->request);
        fc->fetches = g_slist_delete_link(fc->fetches, fc->fetches);
        g_free(fetch->url);
        g_free(fetch);
    }
    g_list_free_full(fc->feeds, (GDestroyNotify)feed_free);
    g_free(fc);
    purple_connection_set_protocol_data(gc, NULL);
}

static int feeds_send_im(PurpleConnection *gc, const char *who,
                         const char *message, PurpleMessageFlags flags)
{
    FeedsConnection *fc = purple_connection_get_protocol_data(gc);

    if (!fc || !g_str_equal(who, NEWS_BUDDY))
        return -ENOTSUP;
    run_command(fc, message);
    return 1;
}

static void add_feed_menu(PurpleBlistNode *node, gpointer data)
{
    show_add_dialog(purple_buddy_get_account((PurpleBuddy *)node));
}

static void remove_feed_menu(PurpleBlistNode *node, gpointer data)
{
    show_remove_dialog(purple_buddy_get_account((PurpleBuddy *)node));
}

static void check_now_menu(PurpleBlistNode *node, gpointer data)
{
    FeedsConnection *fc = connection_of(purple_buddy_get_account((PurpleBuddy *)node));

    if (fc)
        feeds_fetch_all(fc);
}

static GList *feeds_blist_node_menu(PurpleBlistNode *node)
{
    GList *menu = NULL;

    if (!PURPLE_BLIST_NODE_IS_BUDDY(node))
        return NULL;
    menu = g_list_append(menu, purple_menu_action_new(
            "Add feed...", PURPLE_CALLBACK(add_feed_menu), NULL, NULL));
    menu = g_list_append(menu, purple_menu_action_new(
            "Remove feed...", PURPLE_CALLBACK(remove_feed_menu), NULL, NULL));
    menu = g_list_append(menu, purple_menu_action_new(
            "Check now", PURPLE_CALLBACK(check_now_menu), NULL, NULL));
    return menu;
}

static void add_feed_action(PurplePluginAction *action)
{
    show_add_dialog(purple_connection_get_account(action->context));
}

static void remove_feed_action(PurplePluginAction *action)
{
    show_remove_dialog(purple_connection_get_account(action->context));
}

static void check_all_action(PurplePluginAction *action)
{
    FeedsConnection *fc = purple_connection_get_protocol_data(action->context);

    if (fc)
        feeds_fetch_all(fc);
}

static GList *feeds_actions(PurplePlugin *plugin, gpointer context)
{
    GList *actions = NULL;

    actions = g_list_append(actions, purple_plugin_action_new("Add feed...", add_feed_action));
    actions = g_list_append(actions, purple_plugin_action_new("Remove feed...", remove_feed_action));
    actions = g_list_append(actions, purple_plugin_action_new("Check all feeds now", check_all_action));
    return actions;
}

static GHashTable *feeds_account_text_table(PurpleAccount *account)
{
    GHashTable *table = g_hash_table_new(g_str_hash, g_str_equal);

    g_hash_table_insert(table, "login_label", (gpointer)"Account name...");
    return table;
}

static PurplePluginProtocolInfo prpl_info = {
    .options = OPT_PROTO_NO_PASSWORD,
    .icon_spec = NO_BUDDY_ICONS,
    .list_icon = feeds_list_icon,
    .status_text = feeds_status_text,
    .tooltip_text = feeds_tooltip_text,
    .status_types = feeds_status_types,
    .blist_node_menu = feeds_blist_node_menu,
    .login = feeds_login,
    .close = feeds_close,
    .send_im = feeds_send_im,
    .struct_size = sizeof(PurplePluginProtocolInfo),
    .get_account_text_table = feeds_account_text_table,
};

static void plugin_init(PurplePlugin *plugin)
{
    prpl_info.protocol_options = g_list_append(prpl_info.protocol_options,
            purple_account_option_int_new("Check every (minutes)", OPT_INTERVAL,
                                          DEFAULT_INTERVAL_MINUTES));
    prpl_info.protocol_options = g_list_append(prpl_info.protocol_options,
            purple_account_option_int_new("Articles to show from a new feed",
                                          OPT_INITIAL_ITEMS, DEFAULT_INITIAL_ITEMS));
}

static PurplePluginInfo info = {
    PURPLE_PLUGIN_MAGIC,
    PURPLE_MAJOR_VERSION,
    PURPLE_MINOR_VERSION,
    PURPLE_PLUGIN_PROTOCOL,
    NULL,
    0,
    NULL,
    PURPLE_PRIORITY_DEFAULT,
    FEEDS_PLUGIN_ID,
    "Feeds",
    FEEDS_VERSION,
    "RSS and Atom feeds as a buddy",
    "Collects RSS and Atom feeds into one News buddy; new articles arrive as messages.",
    "Anton Relin",
    "https://github.com/relisher/purple-feeds",
    NULL,
    NULL,
    NULL,
    NULL,
    &prpl_info,
    NULL,
    feeds_actions,
    NULL, NULL, NULL, NULL
};

PURPLE_INIT_PLUGIN(feeds, plugin_init, info);
