# Play as you download: Sion test kit

Plays an Xbox disc image from the Internet Archive while it downloads. A
test beside the installed system: everything goes in /data/frolick/stream,
nothing in /opt/xbcompat or the store service changes.

| file | what it is |
|---|---|
| xbcompat | the Sion's installed combined build (push-liveguide, gudfcx ff23b324, md5 8c1df186… when built without this change) plus streamed images (src/kernel/dvd.c from frolick-xap-dash 8df79fb1); Sion interpreter /opt/xbcompat/lib/ld-linux.so.2 |
| frolick-store | the store helper with `-stream` (static x86-64); run under the name frolick-store-stream, so the store service is never touched |
| stream-play.sh | the test (see its header) |

Install: copy the three files to /data/frolick/stream (mkdir it first) and
`chmod 755` them. Check with `sha256sum -c SHA256SUMS` there.

Run, only while the dashboard alone is on the screen (the script refuses
otherwise): `sh /data/frolick/stream/stream-play.sh` for Half-Life 2
(xbox_essentials, 2,059,796,480 bytes, md5 510f7b04…, sha1 fae0a9a3…).
It stops the dashboard's session and xbcompat (xbox-av keeps the screen),
plays the game from /data/frolick/stream/hl2/disc.iso as it fills in, and
starts the dashboard again when the game quits. Results:
/data/frolick/stream/hl2/result.txt (gate, first frame, bytes fetched by
then), logs fetch.log and xbcompat.log beside it, the read trace
disc.iso.trace.

`stream-play.sh --fetch-only` runs the download alone (gate time, speed
from the Archive, the md5/sha1 check) and doesn't touch the dashboard.

Undo: `rm -r /data/frolick/stream`.
