string includes trio printf library (version 1.16, retrieved 05 August 2014).
The changes (marked with // NOTE: string: blocks) include making trio compile
without warnings and embedding trionan.c and triostr.c into trio.c (to make
integration easier).

No configuration is required, just copy all trio*.* files.

Original README follows.

Trio is a package with portable string functions. Including printf() clones
and others.

 Copyright (C) 1998-2001 by Bjorn Reese and Daniel Stenberg.

 Permission to use, copy, modify, and distribute this software for any
 purpose with or without fee is hereby granted, provided that the above
 copyright notice and this permission notice appear in all copies.

 THIS SOFTWARE IS PROVIDED ``AS IS'' AND WITHOUT ANY EXPRESS OR IMPLIED
 WARRANTIES, INCLUDING, WITHOUT LIMITATION, THE IMPLIED WARRANTIES OF
 MERCHANTIBILITY AND FITNESS FOR A PARTICULAR PURPOSE. THE AUTHORS AND
 CONTRIBUTORS ACCEPT NO RESPONSIBILITY IN ANY CONCEIVABLE MANNER.

Trio is intended to be an integral part of another application, so we
have not done anything to create a proper installation.

Compile with 'make' (edit the Makefile if you want a release build)

Test the package with 'make test'

Install by copying trio.h, triop.h, and libtrio.a (and man/man?/* if
you want documentation) to the appropriate directories.

Catch some usage examples in example.c

Send feedback and patches to the mailing list, subscription and other
information is found here:

        http://lists.sourceforge.net/lists/listinfo/ctrio-talk

Enjoy!

Trio web page

        http://daniel.haxx.se/trio/
