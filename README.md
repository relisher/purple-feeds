# purple-feeds

A protocol plugin for Pidgin and other libpurple clients that reads RSS and
Atom feeds. Each Feeds account has one buddy, **News**, and new articles from
all of its feeds arrive as messages from that buddy:

> **NYT Top Stories:** Senate Republicans Block Bid to Rein in Trump on Iran War

The headline links to the article. For separate conversations per topic, add
more than one Feeds account.

## Building

You need the libpurple 2.x development files, GLib, `pkg-config` and a C
compiler. On macOS with Homebrew, `brew install pidgin pkgconf` provides all
of them.

```sh
make
make install          # copies libfeeds.so to ~/.purple/plugins
```

`make install-system` installs into libpurple's own plugin directory instead.
Restart Pidgin after installing.

## Setting up

Add an account with the **Feeds** protocol. The account name is only a label;
there is no password. The News buddy appears in a News group when the account
connects.

## Adding and removing feeds

From a menu: right-click News, or open the account's menu under
Accounts, and choose **Add feed...** or **Remove feed...**. Adding asks for
the feed address, an optional name (the feed's own title is used without
one) and an optional link filter.

Or send a message to News:

| Message | Effect |
| --- | --- |
| `add <url> [name]` | Adds a feed |
| `remove <name or url>` | Removes a feed |
| `filter <name or url> [regex]` | Shows only articles whose link matches the regex; leaving it out clears the filter |
| `list` | Lists the feeds, their filters and any failing ones |
| `check` | Checks every feed now |
| anything else | Shows this list |

A link filter helps with feeds that mix kinds of articles. For example,
`filter Meduza meduza\.io/news/` keeps only news items.

A new feed shows its five newest articles. After that, only articles the
plugin has not seen before are shown. Seen article IDs are stored in
`~/.purple/feeds/`; the feed list is stored on the account.

## Options

Account options (Modify Account → Advanced):

- **Check every (minutes)**: how often all feeds are fetched. Default 10.
- **Articles to show from a new feed**: default 5.

Hovering over News lists each feed and whether its last check succeeded.

## Testing the parser

`make check FEEDS="one.xml two.xml"` prints the title, item count, and newest
item the parser finds in each saved feed file.

## License

GPL-2.0-or-later, the same as libpurple. See `LICENSE`.
