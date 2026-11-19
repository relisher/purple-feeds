# purple-feeds

A protocol plugin for Pidgin and other libpurple clients that reads RSS and
Atom feeds. Each feed is a buddy; each new article arrives as a message from
that buddy, with the headline linked to the article.

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

1. Add an account with the **Feeds** protocol. The account name is only a
   label; there is no password.
2. Add a buddy to that account for each feed. Enter the feed URL as the
   buddy's username, and optionally an alias. Without an alias, the buddy
   takes the feed's own title.

A new feed shows its five newest articles. After that, only articles the
plugin has not seen before are shown. Seen article IDs are stored in
`~/.purple/feeds/`.

## Options

Account options (Modify Account → Advanced):

- **Check every (minutes)**: how often all feeds are fetched. Default 10.
- **Articles to show from a new feed**: default 5.

Buddy menu (right-click a feed):

- **Check now** fetches that feed immediately.
- **Set link filter...** shows only articles whose link matches a regular
  expression, for example `meduza\.io/news/` to keep only news items from a
  feed that also carries features.

The account menu has **Check all feeds now**.

A feed whose last fetch failed shows as offline, with the error in its
status and tooltip.

## Testing the parser

`make check FEEDS="one.xml two.xml"` prints the title, item count, and newest
item the parser finds in each saved feed file.

## License

GPL-2.0-or-later, the same as libpurple. See `LICENSE`.
