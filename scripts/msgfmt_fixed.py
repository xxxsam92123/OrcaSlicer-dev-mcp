#! /usr/bin/env python3
# Written by Martin v. Löwis <loewis@informatik.hu-berlin.de>

"""Generate binary message catalog from textual translation description.

This program converts a textual Uniforum-style message catalog (.po file) into
a binary GNU catalog (.mo file).  This is essentially the same function as the
GNU msgfmt program, however, it is a simpler implementation.  Currently it
does not handle plural forms but it does handle message contexts.

Usage: msgfmt.py [OPTIONS] filename.po

Options:
    -o file
    --output-file=file
        Specify the output file to write to.  If omitted, output will go to a
        file named filename.mo (based off the input file name).

    -h
    --help
        Print this message and exit.

    -V
    --version
        Display version information and exit.


OrcaSlicer notes
----------------
This file is a patched copy of CPython's Tools/i18n/msgfmt.py (3.9 lineage),
kept in the repository because some build environments (for example Windows
without a GNU gettext installation) have no `msgfmt` binary.

The stock script never clears the pending `msgctxt` when a new entry starts
with `msgid`, so a bare entry that follows a `msgctxt` entry with no
intervening comment line is written to the .mo as `context\x04msgid`. Runtime
lookups without a context then miss it and the UI falls back to English. In
this repository's zh_CN catalog that affected 483 strings ("Flow ratio",
"Bed type", "Copy", "Cut", "Scale", "Help", "Diameter", ...) although the .po
had a translation for every one of them.

The fix is the two added lines marked `FIX:` below.

Usage:
    python scripts/msgfmt_fixed.py -o resources/i18n/zh_CN/OrcaSlicer.mo \
        localization/i18n/zh_CN/OrcaSlicer_zh_CN.po

The .mo has to be updated in the resource directory the running binary reads
(`<exe dir>/resources/i18n/<lang>/OrcaSlicer.mo`); in a build tree that path
is usually a symlink to `resources/i18n/<lang>/`.

Verification after compiling: the number of keys containing `\x04` in the .mo
must equal the number of `msgctxt` entries in the .po, and no context may have
fewer entries in the .mo than in the .po.
"""

import os
import sys
import ast
import getopt
import struct
import array
from email.parser import HeaderParser

__version__ = "1.2"

MESSAGES = {}


def usage(code, msg=''):
    print(__doc__, file=sys.stderr)
    if msg:
        print(msg, file=sys.stderr)
    sys.exit(code)


def add(ctxt, id, str, fuzzy):
    "Add a non-fuzzy translation to the dictionary."
    global MESSAGES
    if not fuzzy and str:
        if ctxt is None:
            MESSAGES[id] = str
        else:
            MESSAGES[b"%b\x04%b" % (ctxt, id)] = str


def generate():
    "Return the generated output."
    global MESSAGES
    # the keys are sorted in the .mo file
    keys = sorted(MESSAGES.keys())
    offsets = []
    ids = strs = b''
    for id in keys:
        # For each string, we need size and file offset.  Each string is NUL
        # terminated; the NUL does not count into the size.
        offsets.append((len(ids), len(id), len(strs), len(MESSAGES[id])))
        ids += id + b'\0'
        strs += MESSAGES[id] + b'\0'
    output = ''
    # The header is 7 32-bit unsigned integers.  We don't use hash tables, so
    # the keys start right after the index tables.
    # translated string.
    keystart = 7*4+16*len(keys)
    # and the values start after the keys
    valuestart = keystart + len(ids)
    koffsets = []
    voffsets = []
    # The string table first has the list of keys, then the list of values.
    # Each entry has first the size of the string, then the file offset.
    for o1, l1, o2, l2 in offsets:
        koffsets += [l1, o1+keystart]
        voffsets += [l2, o2+valuestart]
    offsets = koffsets + voffsets
    output = struct.pack("Iiiiiii",
                         0x950412de,       # Magic
                         0,                 # Version
                         len(keys),         # # of entries
                         7*4,               # start of key index
                         7*4+len(keys)*8,   # start of value index
                         0, 0)              # size and offset of hash table
    output += array.array("i", offsets).tobytes()
    output += ids
    output += strs
    return output


def make(filename, outfile):
    ID = 1
    STR = 2
    CTXT = 3

    # Compute .mo name from .po name and arguments
    if filename.endswith('.po'):
        infile = filename
    else:
        infile = filename + '.po'
    if outfile is None:
        outfile = os.path.splitext(infile)[0] + '.mo'

    try:
        with open(infile, 'rb') as f:
            lines = f.readlines()
    except IOError as msg:
        print(msg, file=sys.stderr)
        sys.exit(1)

    section = msgctxt = None
    fuzzy = 0

    # Start off assuming Latin-1, so everything decodes without failure,
    # until we know the exact encoding
    encoding = 'latin-1'

    # Parse the catalog
    lno = 0
    for l in lines:
        l = l.decode(encoding)
        lno += 1
        # If we get a comment line after a msgstr, this is a new entry
        if l[0] == '#' and section == STR:
            add(msgctxt, msgid, msgstr, fuzzy)
            section = msgctxt = None
            fuzzy = 0
        # Record a fuzzy mark
        if l[:2] == '#,' and 'fuzzy' in l:
            fuzzy = 1
        # Skip comments
        if l[0] == '#':
            continue
        # Now we are in a msgid or msgctxt section, output previous section
        if l.startswith('msgctxt'):
            if section == STR:
                add(msgctxt, msgid, msgstr, fuzzy)
            section = CTXT
            l = l[7:]
            msgctxt = b''
        elif l.startswith('msgid') and not l.startswith('msgid_plural'):
            if section == STR:
                add(msgctxt, msgid, msgstr, fuzzy)
                if not msgid:
                    # See whether there is an encoding declaration
                    p = HeaderParser()
                    charset = p.parsestr(msgstr.decode(encoding)).get_content_charset()
                    if charset:
                        encoding = charset
            # FIX: 新条目若没有 msgctxt 行，必须清空上一条目遗留的 msgctxt
            if section != CTXT:
                msgctxt = None
            section = ID
            l = l[5:]
            msgid = msgstr = b''
            is_plural = False
        # This is a message with plural forms
        elif l.startswith('msgid_plural'):
            if section != ID:
                print('msgid_plural not preceded by msgid on %s:%d' % (infile, lno),
                      file=sys.stderr)
                sys.exit(1)
            l = l[12:]
            msgid += b'\0' # separator of singular and plural
            is_plural = True
        # Now we are in a msgstr section
        elif l.startswith('msgstr'):
            section = STR
            if l.startswith('msgstr['):
                if not is_plural:
                    print('plural without msgid_plural on %s:%d' % (infile, lno),
                          file=sys.stderr)
                    sys.exit(1)
                l = l.split(']', 1)[1]
                if msgstr:
                    msgstr += b'\0' # Separator of the various plural forms
            else:
                if is_plural:
                    print('indexed msgstr required for plural on  %s:%d' % (infile, lno),
                          file=sys.stderr)
                    sys.exit(1)
                l = l[6:]
        # Skip empty lines
        l = l.strip()
        if not l:
            continue
        l = ast.literal_eval(l)
        if section == CTXT:
            msgctxt += l.encode(encoding)
        elif section == ID:
            msgid += l.encode(encoding)
        elif section == STR:
            msgstr += l.encode(encoding)
        else:
            print('Syntax error on %s:%d' % (infile, lno), \
                  'before:', file=sys.stderr)
            print(l, file=sys.stderr)
            sys.exit(1)
    # Add last entry
    if section == STR:
        add(msgctxt, msgid, msgstr, fuzzy)

    # Compute output
    output = generate()

    try:
        with open(outfile,"wb") as f:
            f.write(output)
    except IOError as msg:
        print(msg, file=sys.stderr)


def main():
    try:
        opts, args = getopt.getopt(sys.argv[1:], 'hVo:',
                                   ['help', 'version', 'output-file='])
    except getopt.error as msg:
        usage(1, msg)

    outfile = None
    # parse options
    for opt, arg in opts:
        if opt in ('-h', '--help'):
            usage(0)
        elif opt in ('-V', '--version'):
            print("msgfmt.py", __version__)
            sys.exit(0)
        elif opt in ('-o', '--output-file'):
            outfile = arg
    # do it
    if not args:
        print('No input file given', file=sys.stderr)
        print("Try `msgfmt --help' for more information.", file=sys.stderr)
        return

    for filename in args:
        make(filename, outfile)


if __name__ == '__main__':
    main()
