# purple-feeds

An RSS and Atom feed reader plugin for Pidgin and other libpurple-based
messengers. Each Feeds account has one buddy, **News**, and new articles from
all of its feeds arrive as messages from that buddy:

> **NYT Top Stories:** Senate Republicans Block Bid to Rein in Trump on Iran War

The headline links to the article. Add a second Feeds account if you want a
separate conversation for another set of feeds.

## Installation

### Linux

Install the libpurple and GLib development packages, then build and install:

```sh
# Debian/Ubuntu
sudo apt-get install libpurple-dev libglib2.0-dev pkg-config build-essential
# Fedora
sudo dnf install libpurple-devel glib2-devel pkgconf-pkg-config gcc make

git clone https://github.com/relisher/purple-feeds.git
cd purple-feeds
make
sudo make install
```

### macOS

With Homebrew:

```sh
brew install pidgin pkgconf
git clone https://github.com/relisher/purple-feeds.git
cd purple-feeds
make
make install
```

### Just for your user

`make install-user` copies the plugin to `~/.purple/plugins` without
needing root. Pidgin only looks for protocol icons in its system directory,
so the account shows without an icon unless you also run
`make install-icons`.

### Windows

`make libfeeds.dll` cross-compiles with MinGW against a Pidgin 2.x source
tree and the usual win32-dev libraries (set `PIDGIN_TREE_TOP` and
`WIN32_DEV_TOP`). Copy `libfeeds.dll` to Pidgin's `plugins` folder and the
icons to `pixmaps\pidgin\protocols\{16,22,48}\feeds.png`. The Windows build has
not been tested yet.

Restart Pidgin after installing.

## Setup

Add an account (Accounts → Manage Accounts → Add) with the **Feeds**
protocol. The username is only a label and there is no password. The News
buddy appears in a News group when the account connects.

## Adding and removing feeds

From a menu: right-click News, or open the account under Accounts, and
choose **Add feed...** or **Remove feed...**. Adding asks for the feed
address, an optional name (the feed's own title is used without one) and an
optional link filter.

Or send a message to News:

| Message | Effect |
| --- | --- |
| `add <url> [name]` | Adds a feed |
| `remove <name or url>` | Removes a feed |
| `filter <name or url> [regex]` | Shows only articles whose link matches the regex; leaving it out clears the filter |
| `list` | Lists the feeds, their filters and any that are failing |
| `check` | Checks every feed now |
| anything else | Shows the command list |

A link filter helps with feeds that mix kinds of articles. For example,
`filter Meduza meduza\.io/news/` keeps only Meduza's news items.

A new feed shows its five newest articles; after that, only articles the
plugin has not seen before. Seen article IDs are stored in
`~/.purple/feeds/`, and the feed list is stored with the account.

## Options

Under Modify Account → Advanced:

- **Check every (minutes)**: how often all feeds are fetched. Default 10.
- **Articles to show from a new feed**: default 5.

Hovering over News lists each feed and whether its last check succeeded.

## Supported formats

RSS 2.0, RSS 1.0 (RDF) and Atom 1.0. Feeds are fetched with libpurple's own
HTTP client, so the account's proxy settings apply.

## Development

`make check` runs the parser against the feeds in `tests/fixtures` and
compares the result with `tests/expected.txt`.

## License

GPL-2.0-or-later, the same as libpurple. See [LICENSE](LICENSE).
