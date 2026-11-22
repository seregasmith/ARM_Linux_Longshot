# Privacy

Longshot, its launcher and the Test Farm are made by the Magic Bean team, volunteers who look after them in their free
time. This page says what reaches us, why, who can read it, how long we keep it and how to have it removed. Nothing on
it is sent unless you choose to send it.

**Contact:** [privacy@magicbean.dev](mailto:privacy@magicbean.dev), for any question about your data or to ask us to
see, correct or delete it. We answer within a month.

## What Longshot sends

Longshot sends nothing about you until you say so. You choose each of these, and you can stop any of them at any time.

| What | When | What it holds | Who can read it | How long |
|---|---|---|---|---|
| **Crash report** | After a crash, when you press **Send** in the crash window, or every time if you ticked **Send crash reports automatically** in the launcher | The version, your GPU and driver, CPU, OS, the map, your settings, the crash's lines and the 150 log lines before it | The team only | A year, then deleted |
| **Failed build report** | When a build fails and you press **Send to the team** on the launcher's problem page | The launcher's and the game's versions, your CPU, memory and graphics cards, the build's settings and how far it got, and the last 200 lines of its log | The team only | A year, then deleted |
| **Performance statistics** | Only while **Send performance statistics** is ticked in the launcher (off by default) | How frames, hitches and movies went on each map (Link's position rounded to 100 units), how each build ended and how long its steps took, with your GPU, driver, CPU, OS and settings | The team, as totals on our dashboards | Totals, 30 days |

Every report has your user name and home folder taken out, on your PC and again on our server. None of them holds a
user name, an account, an ID, your saves or game files, your key or pad bindings, device names or hardware serials.
Before you press **Send**, the crash window and the problem page show the whole report as it will be sent. With the
performance statistics ticked, **Keep a copy of what was sent** keeps each summary in `perf_sent` in the game folder,
so you can read exactly what went.

**Stopping:** untick **Send crash reports automatically** or **Send performance statistics** in the launcher's
Settings. Unticking the statistics also deletes what is waiting to be sent and the copies kept in `perf_sent`.

### Why we use it

To find and fix crashes, failed builds, stutters and slow areas on PCs we don't have. We use it for nothing else: no
ads, no profiles, and we sell or give it to nobody. Sending is your choice (your consent), and you can take it back
by unticking the box.

### Your address on the internet

Our server, `data-services.magicbean.dev`, is behind Cloudflare, which sees your IP address when a report arrives, as
it does for any website it serves ([Cloudflare's privacy policy](https://www.cloudflare.com/privacypolicy/)). Longshot's
server code never reads or stores your IP address, and none of it is in a report or on our dashboards.

### Things you send yourself

**Report manually** in the crash window, the in-game **Bug report** and **Save my pipeline list** send nothing: they
make files and a filled-in form for you to post yourself. What you post on an issue at git.magicbean.dev is public, like
any issue there.

## The Test Farm

If you lend your PC to the Test Farm, it keeps your git.magicbean.dev login, your PC's name and hardware, when it last
checked in and the results of its tests; BigBeans also see your Discord name. Your login and PC's name show on the
farm's public pages unless you hide yourself, and pull requests show your PC's hardware. A PC not seen for 90 days is
removed, and reports stop naming you after 180 days. The [Test Farm's privacy page](https://beanstalk.magicbean.dev/privacy) says what, for how long and how
to remove your PC and your data.

## What else Longshot connects to

See [what Longshot connects to](updates-and-saves.md#what-longshot-connects-to): downloads from git.magicbean.dev, your
Discord status, and the reports above. That's all.

## Your rights

Where the EU's or the UK's GDPR (or a law like it) applies to you, you can ask us to show you, correct or delete what
we hold about you, to stop using it, or to give you a copy. Write to
[privacy@magicbean.dev](mailto:privacy@magicbean.dev). The reports carry no name or ID, so we usually can't tell which
ones are yours: tell us when you sent one and your version, GPU and OS, and we'll look. You can also complain to your
country's data protection authority.

We'll update this page when what Longshot sends changes. Its history is in
[Longshot's repository](https://git.magicbean.dev/MagicBeanDev/Longshot/commits/branch/main/docs/user/privacy.md).
