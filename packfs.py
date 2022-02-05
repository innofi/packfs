from argparse import ArgumentParser, FileType
from struct import pack, calcsize
from json import loads, dump
from re import match, search
from lzo import compress
from hashlib import sha256
from crcmod.predefined import mkCrcFun

PACKFS_MAGIC = 0x12fc
PACKFS_VERSION = 0x01
PACKFS_LZOBLOCK = 1024
#PACKFS_LZOBLOCK = 1024*2
PACKFS_LZOLEVEL = 9

PACKFS_SIZE_HEADER = calcsize('<HBHHII32BH')
PACKFS_SIZE_META = calcsize('<B32s64s')
PACKFS_SIZE_INDEX = calcsize('<IIB64s')

PT_REG = 0x01
PT_IMG = 0x02
PF_LZO = 0x10

crc16 = mkCrcFun('crc-16-genibus')


def mkheader(metasize, indexsize, regentrysize, imgentrysize, sig):
    d = pack('<HBHHII', PACKFS_MAGIC, PACKFS_VERSION, metasize, indexsize, regentrysize, imgentrysize) + sig
    return d + pack('<H', crc16(d))


def mkmeta(flags, key, value):
    return pack('<B32s64s', flags, key.encode('utf-8'), value.encode('utf-8'))


def etype(flags):
    t = ''
    if flags & PF_LZO: t += 'lzo compressed '
    if flags & PT_IMG: t += 'image file'
    elif flags & PT_REG: t += 'regular file'
    else: t += 'unknown'
    return t


def mkindex(offset, length, flags, name):
    return pack('<IIB64s', offset, length, flags, name.encode('utf-8'))


def mkimghash(data):
    return sha256(data).digest()


def lzopercent(a, b):
    return "incompressible" if a == b else "{}%".format(round(-100.0+100.0*b/a, 2))


def mklzoentry(blocksize, data):
    d = [pack('<IH', len(data), blocksize)]
    chunks = [data[i:i + blocksize] for i in range(0, len(data), blocksize)]
    for i in range(len(chunks)):
        c = chunks[i]
        x = compress(c, PACKFS_LZOLEVEL, False)
        if len(x) >= len(c): x = c
        print("- Compressing block {}... {} -> {} bytes ({})".format(i, len(c), len(x), lzopercent(len(c), len(x))))
        d.extend([pack('<H', len(x)), x])
    e = b''.join(d)
    print("- Overall compression {} -> {} bytes ({})".format(len(data), len(e), lzopercent(len(data), len(e))))
    return e


def mkpack(meta, entries, strip=False):
    metasize = len(meta) * PACKFS_SIZE_META
    indexsize = len(entries) * PACKFS_SIZE_INDEX
    offset = PACKFS_SIZE_HEADER + metasize + indexsize
    print("Adding meta keys {}".format(', '.join(map(lambda x: "[{}]{}={}".format(hex(x[0]), x[1], x[2]), meta))))
    
    d = {'head': [mkmeta(m[0], m[1], m[2]) for m in meta], 'reg': [], 'img': []}
    reg = sorted(filter(lambda e: e['flags'] & PT_REG, entries), key=lambda e: len(e['data']))
    img = sorted(filter(lambda e: e['flags'] & PT_IMG, entries), key=lambda e: len(e['data']))

    def mkentry(entry, section, offset):
        print("Adding {} entry {}".format(etype(entry['flags']), entry['name']))
        if entry['flags'] & PT_IMG: entry['hash'] = mkimghash(entry['data'])
        if entry['flags'] & PF_LZO: entry['data'] = mklzoentry(PACKFS_LZOBLOCK, entry['data'])
        if entry['flags'] & PT_IMG: entry['data'] = entry['hash'] + entry['data']
        length = len(entry['data'])
        d['head'].append(mkindex(offset, length, entry['flags'], entry['name']))
        d[section].append(entry['data'])
        print("- Entry offset {} length {}".format(offset, length))
        return length

    for r in reg: offset += mkentry(r, 'reg', offset)
    for i in img: offset += mkentry(i, 'img', offset)

    sig = sha256()
    for x in d['head'] + d['reg']: sig.update(x)

    regdata = b''.join(d['reg'])
    imgdata = b''.join(d['img'])
    d['head'].insert(0, mkheader(metasize, indexsize, len(regdata), len(imgdata), sig.digest()))

    filedata = b''.join(d['head']) + regdata
    if not strip: filedata += imgdata
    print("=> Total filesize {} bytes".format(len(filedata)))
    return filedata


def main():
    parser = ArgumentParser(description="Generate packfs file archive")
    parser.add_argument('-m', '--meta', action='append', type=str, help="Add meta variables of the form key=value")
    parser.add_argument('-s', '--strip', action='store_true', help="Strip the image section out of the generated file")
    parser.add_argument('-e', '--entry', action='append', type=str, help="Add file entry of the type name=flag1,flag2:path")
    parser.add_argument('-f', '--file', action='append', type=FileType('r'), help="Read manifest json file")
    parser.add_argument('-i', '--index', action='store', type=FileType('w'), help="Output manifest index in json format")
    parser.add_argument('-t', '--template', action='append', type=str, help="Replace template variables in files specified by name=value. Replaces file contents of the type {{name}}.")
    parser.add_argument('-o', '--output', action='store', type=FileType('wb'), help="Output filename of the generated packfile")

    args = parser.parse_args()

    def parsetemplate(vars, content):
        while True:
            m = search('{{\\s*([a-zA-Z0-9_]+)\\s*}}', content)
            if not m: break
            n = m.group(1)
            if n not in vars: raise ValueError("Template variable not set: {}".format(n))
            content = content.replace(m.group(0), vars[n])
        return content

    def parseargtemplate(arg):
        m = match('^([a-zA-Z0-9_]+)=(.*)$', arg)
        if not m: raise ValueError("Bad parse key=value: {}".format(arg))
        return (m.group(1), m.group(2))

    def parsejsonmeta(obj):
        return (0, obj['name'], obj['value'])

    def parseargmeta(arg):
        m = match('^([a-zA-Z0-9_]+)=(.*)$', arg)
        if not m: raise ValueError("Bad parse key=value: {}".format(arg))
        return parsejsonmeta({'name': m.group(1), 'value': m.group(2)})

    def parsejsonentry(obj):
        with open(obj['path'], 'rb') as fp:
            data = fp.read()
        return {
            'name': obj['name'],
            'flags': (PF_LZO if "lzo" in obj['flags'] else 0x0) | (PT_IMG if "img" in obj['flags'] else 0x00) | (PT_REG if "reg" in obj['flags'] else 0x00),
            'sha256': sha256(data).hexdigest(),
            'data': data
        }

    def parseargentry(arg):
        m = match('^([a-zA-Z0-9_/.]+)=([a-z,]+):(.*)$', arg)
        if not m: raise ValueError("Bad parse key=flag1,flag2:value: {}".format(arg))
        return parsejsonentry({'name': m.group(1), 'flags': m.group(2).split(','), 'value': m.group(3)})

    # Parse arguments
    meta = []
    index = []
    output = None
    manifestindex = None
    strip = args.strip

    template = {}
    if args.template is not None:
        for temp in args.template:
            template.update([parseargtemplate(temp)])

    if args.file is not None:
        for file in args.file:
            data = loads(parsetemplate(template, file.read()))
            if 'meta' in data: meta.extend([parsejsonmeta(m) for m in data['meta']])
            if 'entries' in data: index.extend([parsejsonentry(e) for e in data['entries']])
            if 'output' in data: output = open(data['output'], 'wb')
            if 'index' in data: manifestindex = open(data['index'], 'w')

    if args.meta is not None:
        for m in args.meta:
            meta.append(parseargmeta(m))

    if args.entry is not None:
        for e in args.entry:
            index.append(parseargentry(e))

    if args.output is not None:
        output = args.output

    if args.index is not None:
        manifestindex = args.index

    # Generate the index manifest
    if manifestindex is not None:
        dump(dict([(file['name'], file['sha256']) for file in index]), manifestindex)

    # Generate and output packfile
    print("== Writing PACK file {} ==".format(output.name))
    output.write(mkpack(meta, index, strip))
    output.close()

if __name__ == '__main__':
    main()
