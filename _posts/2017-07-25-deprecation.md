---
layout: post
title:  "Deprecation of old parameters and features"
date:   2017-07-25 9:00:00 +0200
author: Thomas Huth
categories: [features, 'web site']
---
QEMU has a lot of interfaces (like command line options or HMP commands) and
old features (like certain devices) which are considered as deprecated
since other more generic or better interfaces/features have been established
instead. While the QEMU developers are generally trying to keep each QEMU
release compatible with the previous ones, the old legacy sometimes gets into
the way when developing new code and/or causes quite some burden of maintaining
it.

Thus we are currently considering to get rid of some of the old interfaces
and features in a future release and have started to collect a list of such
old items in our Wiki on a
[page about removing legacy parts](http://wiki.qemu.org/Features/LegacyRemoval).
If you are running QEMU directly, please have a look at this page to see
whether you are still using one of these old interfaces or features, so you
can adapt your setup to use the new interfaces or features instead. Or if
you rather think that one of the legacy interfaces/features should *not* be
removed from QEMU at all, please speak up on the
[qemu-devel mailing list](http://wiki.qemu.org/Contribute/MailingLists)
to explain why the interface or feature is still required.
