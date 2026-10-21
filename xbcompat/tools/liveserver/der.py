"""Just enough DER for Kerberos (RFC 1510) messages."""
import datetime

SEQ, INT, OCTETS, BITS, GSTR, GTIME = 0x30, 0x02, 0x04, 0x03, 0x1B, 0x18


def tlv(tag, content):
    n = len(content)
    if n < 0x80:
        ln = bytes([n])
    else:
        b = n.to_bytes((n.bit_length() + 7) // 8, 'big')
        ln = bytes([0x80 | len(b)]) + b
    return bytes([tag]) + ln + content


def seq(*items):
    return tlv(SEQ, b''.join(items))


def ctx(n, content):
    """[n] EXPLICIT."""
    return tlv(0xA0 + n, content)


def app(n, content):
    """[APPLICATION n] EXPLICIT."""
    return tlv(0x60 + n, content)


def integer(v):
    n = max(1, (v.bit_length() + 8) // 8)
    return tlv(INT, v.to_bytes(n, 'big', signed=True))


def octets(b):
    return tlv(OCTETS, b)


def gstr(s):
    return tlv(GSTR, s.encode() if isinstance(s, str) else s)


def bits32(v):
    return tlv(BITS, b'\0' + v.to_bytes(4, 'big'))


def ktime(t):
    return tlv(GTIME, t.strftime('%Y%m%d%H%M%SZ').encode())


def fields(*pairs):
    """SEQUENCE of [n] EXPLICIT fields, skipping None values."""
    return seq(*(ctx(n, v) for n, v in pairs if v is not None))


# ---- decoding --------------------------------------------------------------

def read(data, pos=0):
    """(tag, content, raw_tlv, next_pos)."""
    tag = data[pos]
    ln = data[pos + 1]
    p = pos + 2
    if ln & 0x80:
        k = ln & 0x7F
        ln = int.from_bytes(data[p:p + k], 'big')
        p += k
    return tag, data[p:p + ln], data[pos:p + ln], p + ln


def items(content):
    out = []
    p = 0
    while p < len(content):
        tag, c, raw, p = read(content, p)
        out.append((tag, c, raw))
    return out


def unwrap(data):
    """Content of the outermost TLV."""
    return read(data)[1]


def ctx_fields(seq_content):
    """{n: (inner content, inner raw)} for a SEQUENCE of [n] EXPLICIT fields."""
    out = {}
    for tag, c, _ in items(seq_content):
        if 0xA0 <= tag <= 0xBE:
            itag, ic, iraw, _ = read(c)
            out[tag - 0xA0] = (ic, iraw)
    return out


def as_int(c):
    return int.from_bytes(c, 'big', signed=True)


def as_time(c):
    return datetime.datetime.strptime(c.decode()[:14], '%Y%m%d%H%M%S').replace(tzinfo=datetime.timezone.utc)
