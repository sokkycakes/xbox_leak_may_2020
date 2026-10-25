# Xbox Live (2002 leak client) — what a replacement server must implement

Source: `xbox trunk/xbox/private/...` in the May-2020 leak. All paths below are relative to
`private/` unless stated. Line numbers are from the leaked files as-is.

Scope: the dashboard's path — machine-only `XOnlineLogon` → MACS → KDC (AS, TGS) → Security
Gateway (SG) key exchange → XRL HTTP calls to the user-account service (`/xuacs/*.ashx`).

---

## 0. Build flavour notes (matter for "which key does the client use")

| lib | defines | used by |
|---|---|---|
| `xonline.lib` (LIBTAG X) | `XONLINE_FEATURE_XBOX` + `XONLINE_FEATURE_INSECURE` | **leak dashboard** (`online/xodash/XODashMain/sources:64`, `-DXONLINE_BUILD_LIBX`) and devkit titles |
| `xonlines.lib` (LIBTAG S) | `XONLINE_FEATURE_XBOX` only | secure/retail titles |
| `xneto.lib` (XNET_BUILD_LIBO) | ONLINE + SG + **INSECURE** | pairs with xonline.lib |
| `xnetos.lib` (XNET_BUILD_LIBOS) | ONLINE + SG, no INSECURE | pairs with xonlines.lib |

(`online/xonp.h:35-58`, `online/sources.inc:30-41`, `ntos/net/xnp.h:66-170`)

**Important:** in the leaked `online/logon.cpp` the EEPROM read of the online key is compiled
out — `#ifdef TODO_REALLY_READ_ONLINE_KEY_FROM_EEPROM` is never defined anywhere in the tree.
Both secure and insecure builds of *this* source therefore use a hard-coded devkit identity for
MACS (`logon.cpp:763-798`):

* serial = `"999999999999"`
* online key = `DEVKIT_ONLINE_RAND_KEY` = `7B 59 2E 4F 81 78 B4 C7 57 88 53 1B 2E 74 76 87`
  (`inc/cryptkeys.h:35`), used **unmorphed**.

A shipped retail title built from a later tree almost certainly had the EEPROM path enabled;
see §6.1.

---

## 1. Overview sequence (machine-only logon, dashboard)

The dashboard calls `XOnlineLogon(Users = all-zero[4], {XONLINE_USER_ACCOUNT_SERVICE=5,
XONLINE_BILLING_OFFERING_SERVICE=4}, 2, ...)` (`online/xodash/XODashLib/NetConfig.cpp:595-601`).
`XOnlineLogon` always adds `XONLINE_MESSAGE_SERVICE=2`; presence (1) is added only if users exist
(`logon.cpp:3043-3058`). Requested service IDs end up in index order **{2, 4, 5}**.

```
Xbox                                   Server(s)
 |  DNS A  macs.xbox.com                 (DNS you control)
 |-- UDP:88 AS-REQ (cname=999999999999@macs.xbox.com, sname=krbtgt/macs.xbox.com) --> MACS
 |<- AS-REP + PA 203 (XKERB_PA_XBOX_ACCOUNT_CREATION: new machine account) ---------|
 |   (stored in HDD config sector 1; skipped on later logons if present & valid)
 |  DNS A  passport.as.xbox.com
 |-- UDP:88 AS-REQ (cname=<machine name>@<machine realm>, sname=krbtgt/xbox.com) --> "Passport AS"
 |<- AS-REP (TGT for xbox.com) ------------------------------------------------------|
 |  DNS A  kdc.xbox.com
 |-- UDP:88 TGS-REQ (sname=sg/S2 @xbox.com, PA-TGS-REQ + PA 201 service request) --> Xbox KDC
 |<- TGS-REP (service ticket + PA 202: site IP, per-service hr + port) ----------------|
 |   logon task completes here: XONLINE_S_LOGON_CONNECTION_ESTABLISHED
 |   (no presence service => XnLogon(0) => no SG connection yet)
 |
 |  _XOnlineGetTags(): XRL POST to fake secure IP for service 5 -> TCP SYN queued
 |-- UDP 3074->3074 KeyEx XbToSg INIT (SPI=0; Init|DH g^X|Kerberos AP-REQ) ---------> SG (siteIP)
 |<- UDP KeyEx SgToXb RESP (Resp|DH g^Y|Kerberos AP-REP) ----------------------------|
 |== ESP-in-UDP 3074 (HMAC-SHA1-96 + DES/3DES-CBC) carrying raw TCP to port wServicePort ==|
 |-- POST /xuacs/gettags.ashx HTTP/1.0 ; Content-Type: xon/5 ; binary body ---------> xuacs
 |<- HTTP 200, Content-Length, binary body -------------------------------------------|
```

Timeouts/retries: Kerberos 4 tries, 4/8/16/16 s; MACS 4 tries, 8/16/32/32 s, rotating over the
DNS A records (`logon.cpp:49-52`); whole logon capped at 90 s (`logon.cpp:54`). SG key exchange:
4 retransmits, 2 s apart (`ntos/net/xnp.h:445-446`).

All Kerberos traffic is **UDP only, port 88**, single datagram each way, client receive buffer
1460 bytes (`logon.cpp:20-21,44,423,896,1809`). There is no TCP fallback; `KRB_ERR_RESPONSE_TOO_BIG`
is fatal. The sockets are flagged `XnSetInsecure` (plain UDP, not ESP).

---

## 2. Common Kerberos details (all steps)

* ASN.1: Microsoft's generated BER/DER codec (`online/krb5.cpp`, `krb5.h`, `msasn1.cpp`) for the
  standard RFC 1510 PDUs plus MS types `KERB_PA_PAC_REQUEST_EX` (`krb5.h:534`) and
  `KERB_PA_COMPOUND_IDENTITY` = SEQUENCE OF Ticket (`krb5.h:840`). PKINIT types exist in the
  module but the client never sends them. A standard Kerberos ASN.1 library (e.g. impacket/pyasn1)
  is sufficient on the server.
* **Only etype 23 (RC4-HMAC, `KERB_ETYPE_RC4_HMAC_NT`)**; etype list in every request is `{23}`
  (`kerberos.cpp:938, 2161`). Session keys in replies **must** be keytype 23, length 16 or the
  client fails (`kerberos.cpp:2438, 3034`).
* RC4-HMAC = RFC 4757 without usage translation (`kerberos.cpp:4547-4734`):
  `K1 = HMAC-MD5(key, LE32(usage))`; `checksum = HMAC-MD5(K1, confounder8||plain)`;
  `K3 = HMAC-MD5(K1, checksum)`; RC4(K3) over confounder||plain. Wire = `checksum16 || enc(confounder8||plain)`
  (header `RC4_MDx_HEADER`, `kerberos.cpp:19-22`, overhead 24 bytes).
* Key usages ("salts") actually used (`kerberos.cpp:295-316`, `inc/xonlinep.h:2749-2753`):

| what | usage |
|---|---|
| PA-ENC-TIMESTAMP | 1 |
| AS-REP enc-part | **8** (the code intends 3, but passes `KERB_ENCRYPTED_AS_REPLY_PDU`, so the ternary at `kerberos.cpp:3697` picks `KERB_TGS_REP_SALT`=8 — i.e. same as RFC 4757 / Windows) |
| TGS-REP enc-part | 8 |
| TGS AP-REQ authenticator | 7 |
| SG AP-REQ authenticator | 11 |
| AP-REP enc-part | 12 |
| Ticket enc-part (server-side only) | 2 (server's choice; client never opens tickets) |
| PA 132 client version | 1027 |
| PA 201 service request | 1201 |
| PA 202 service address | 1202 |
| PA 203 account creation | 1203 |

* Xbox "nonce HMAC key" used for the Xbox PAs (`KerbComputeKdcRequestNonceHmacKey`, `kerberos.cpp:1155-1196`):
  ```
  Ksign = HMAC-MD5(K, "signaturekey\0")            # 13 bytes incl. NUL
  Knonce = HMAC-MD5(Ksign, MD5(LE32(1026) || LE32(nonce)))
  ```
  The PA payload is then RC4-HMAC(Knonce, usage, struct) packed as an ASN.1 `EncryptedData`
  (etype 23). Cipher length is checked **exactly** (24 + sizeof struct).
* Principal names: client principal is one component, type `KRB_NT_ENTERPRISE_PRINCIPAL` (10),
  string `name[.kingdom][@domain]` (`kerberos.cpp:2124-2144`). Server principals are two components
  of type `KRB_NT_SRV_INST` (2). KDC options = `name-canonicalize` (0x00010000). `till` =
  "never" time; `rtime` set in TGS. The client copies cname/crealm **from the reply** for use in
  later authenticators, so the KDC may canonicalize.
* Clock skew: a `KRB-ERROR` with `KRB_AP_ERR_SKEW` (37) and `stime/susec` makes the client call
  `NtSetSystemTime` with the server time and retry, up to 4 times (`kerberos.cpp:2286-2306,
  2850-2866`; `KerbUpdateCurrentTime` 4225). Useful for emulators with a wrong RTC.
* Any other `KRB-ERROR` maps to an NTSTATUS via `KerbMapKerbError` (`kerberos.cpp:4101`) and fails the step.
* Ticket size after the client re-encodes it must be ≤ 1024 bytes (`XONLINE_MAX_TICKET_LENGTH`,
  `kerberos.cpp:2391, 2987`). Tickets are stored opaque; encode them as plain RFC 1510 `Ticket`.

---

## 3. Step 1+2 — MACS: get a machine account (`GetMachineAccount`)

Code: `logon.cpp:735-1215`, request builder `CXo::XkerbBuildAsRequest` `kerberos.cpp:1963-2226`,
reply `CXo::XkerbVerifyAsReply` `kerberos.cpp:2232-2500`, PA check `KerbVerifyAccountCreationPreAuth`
`kerberos.cpp:1201-1295`.

**When it runs:** only if HDD config sector 1 (`MACHINE_ACCOUNT_CONFIG_SECTOR`) does not hold a
valid machine account (`logon.cpp:1162-1176`), or later steps say the account is unknown
(`XONLINE_E_LOGON_MACHINE_AUTHENTICATION_FAILED` from AS, or `XONLINE_E_LOGON_XBOX_ACCOUNT_INVALID`
= 0x8015100D in the PA 202 `hr`; `logon.cpp:1399, 2485-2503, 2718-2742`).

**Transport:** UDP → `macs.xbox.com`:88 (`MACS_KDC_DNS`, `online/xonp.h:3663`; `logon.cpp:1183`).

**Request (AS-REQ, msg-type 10):**
* `cname` = `"999999999999"` (leak build) / 12-char EEPROM serial (EEPROM build), type 10
* `realm` = `"macs.xbox.com"` (`MACS_REALM`, `xonp.h:570`), `sname` = `krbtgt/macs.xbox.com`
  (`fMachineAccount` is FALSE here, so target realm = user realm, `kerberos.cpp:2146`)
* client key K = online key (16 bytes, used directly as the RC4-HMAC key)
* padata, in wire order:
  1. **132 PA-CLIENT-VERSION**: `"Xbox Version=<ver> Title=0x<id> TitleVersion=<n>\0"` encrypted
     with Knonce(K, nonce) usage 1027. *Bug:* it is built before the nonce is randomized, so it uses
     nonce **0** (`kerberos.cpp:2086` vs `2110`). Server can ignore it.
  2. **2 PA-ENC-TIMESTAMP** with K, usage 1 (verify to authenticate the box).
  3. **131 PA-PAC-REQUEST-EX** `{include-pac TRUE, pac-sections {13,14}}` — ignore.
* `nonce` random 32-bit; remembered (up to 8 across retries/skew retries, all are accepted).

**Reply the server must send (AS-REP, msg-type 11):**
* `padata` must contain **203 PA-XBOX-ACCOUNT-CREATION** (else `XONLINE_E_LOGON_MACS_FAILED`,
  `logon.cpp:1010-1015`): `EncryptedData{etype 23, cipher = RC4-HMAC(Knonce(K, req.nonce), 1203,
  XKERB_PA_XBOX_ACCOUNT_CREATION)}`, cipher length exactly 24+96 = 120.
  ```c
  // inc/xonlinep.h:2829, #pragma pack(1), 96 bytes
  ULONGLONG qwUserID;     // machine XUID (becomes m_qwMachineId / XboxID)
  CHAR name[16];          // machine principal name
  CHAR kingdom[12];       // optional, appended as ".kingdom"
  CHAR domain[20];        // optional, appended as "@domain"
  CHAR realm[24];         // realm for the machine AS-REQ (e.g. "passport.net" or anything)
  BYTE key[16];           // machine account key (server-chosen random)
  ```
* `ticket`: anything decodable (it is never used afterwards).
* `enc-part`: `EncASRepPart` under K, usage 8, with the request `nonce`, `key` = any 16-byte etype-23
  key, `starttime`, `endtime`; `cname/crealm` in the outer reply.

Client then stores the account (key encrypted with the HDD key, struct HMAC-signed) in config
sector 1 (`users.cpp:352-398`).

**Server must know:** K (online key) for that serial. In the leak build it is the public devkit
constant. See §6.1 for retail.

---

## 4. Step 3 — KDC: machine TGT (AS) and service ticket (TGS)

### 4.1 AS to "Passport" (`GetSingleTicket`, `logon.cpp:323-728`; called from `GetCombinedTicket`, `logon.cpp:1344-1438`)

With zero users, the combined ticket is just one AS exchange for the machine account
(`fMachineAccount=TRUE`, no PA-COMPOUND-IDENTITY). Cached on disk (`XONLC_TYPE_COMBINED_USER_TICKETS`)
until `endtime`; a cache hit skips this exchange.

* UDP → `passport.as.xbox.com`:88 (`PASSPORT_AS_DNS`, `xonp.h:3659`; `logon.cpp:664`).
* `cname` = `machine.name[.kingdom][@domain]` (type 10), `realm` = machine `realm` from PA 203,
  `sname` = **`krbtgt/xbox.com`** (cross-realm TGT; `kerberos.cpp:2146`, `XBOX_REALM` `xonp.h:569`).
* padata: 132 (nonce 0 bug as above), 2 PA-ENC-TIMESTAMP with the **machine key**, 131 PAC request.
* Reply: AS-REP, enc-part under machine key usage 8, nonce echo, session key etype 23/16 bytes.
  No PA required. The ticket is opaque to the client; encrypt it with a key only your TGS knows.
* With users present (not the dashboard case): each user does AS for `krbtgt/<user realm>` to the
  same host; each subsequent AS carries PA 130 COMPOUND-IDENTITY (previous TGT) and both the
  timestamp and the reply are keyed with
  `Kc = HMAC-MD5(HMAC-MD5(K,"signaturekey\0"), MD5(LE32(1024)||prevTGTSessionKey))`
  (`kerberos.cpp:1736-1781, 2010-2040`). The machine AS is last and yields the combined TGT.

### 4.2 TGS to the Xbox KDC (`GetServiceTicket`, `logon.cpp:1590-2110`; builder `kerberos.cpp:2504-2770`; reply check `kerberos.cpp:2775-3095`)

* UDP → `kdc.xbox.com`:88 (`XBOX_KDC_DNS`, `xonp.h:3662`; `logon.cpp:2053`).
* `sname` = **`sg/S<first service id>`** (type 2), i.e. `sg/S2` for the dashboard;
  `realm` = `xbox.com` (TGT target realm). One TGS covers all requested services.
* padata, wire order:
  1. **201 PA-XBOX-SERVICE-REQUEST**: RC4-HMAC(Knonce(TGT session key, req.nonce), 1201, struct), 112 bytes:
     ```c
     // inc/xonlinep.h:2778, pack(1)
     WORD  wServiceRequestVersion;   // 1
     BYTE  abEthernetAddr[6];        // EEPROM MAC (XC_FACTORY_ETHERNET_ADDR)
     WORD  clientVersion[4];         // major, minor, build, qfe
     DWORD dwTitleID, dwTitleVersion, dwTitleRegion;
     ULONGLONG qwUserID[4];          // all 0 for machine logon
     DWORD dwNumServices;            // 3
     DWORD dwServiceID[12];          // {2,4,5}
     ```
  2. **1 PA-TGS-REQ** = AP-REQ (ap-options 0) with the TGT; authenticator encrypted with the TGT
     session key usage 7, `seq-number` = request nonce, checksum type **7 (plain MD5)** of the
     DER-encoded `req-body` (`kerberos.cpp:940-1000`).
  3. **131** PA-PAC-REQUEST-EX `{include-pac FALSE}` — ignore.
* **Reply (TGS-REP, msg-type 13):**
  * `padata` **must** contain **202 PA-XBOX-SERVICE-ADDRESS** (`kerberos.cpp:2878-2891`), encrypted
    with Knonce(TGT session key, req.nonce), usage 1202, cipher length exactly 24+188 = 212:
    ```c
    // inc/xonlinep.h:2813, pack(1), 188 bytes
    HRESULT hr;                       // S_OK; 0x8015100D => client recreates machine account
    HRESULT hrUser[4];                // per-user result (0 for no users)
    DWORD   dwUserFlags[4];
    IN_ADDR siteIPAddress;            // IP of the SG (network byte order)
    DWORD   dwNumServices;            // must equal the request count
    struct { DWORD dwServiceID; HRESULT hr; WORD wServicePort; WORD wReserved; } serviceResult[12];
                                      // hr S_OK + port behind SG; S_FALSE = "other site" (avoid)
    ```
  * `enc-part`: EncTGSRepPart under TGT session key usage 8, nonce echo, new session key
    (etype 23/16) = the key the SG will use.
  * `ticket`: service ticket for `sg/S2@xbox.com`, encrypted with the SG's own key. The real KDC put
    `KERB_AUTH_DATA_XBOX` (ad-type 200, `XKERB_AD_XBOX`, `inc/xonlinep.h:2839-2859`) in the
    ticket's authorization-data so the SG learned XboxID/users/title/services. The client never
    reads it — a replacement can put whatever its SG needs (the insecure `XkerbGetAuthData`,
    `logon.cpp:3356-3395`, shows the intended contents).
* On success the client generates its DH secret X (96 random bytes) and g^X, copies the service
  session key into `XOKERBINFO._abKey`, and maps `siteIPAddress`+serviceID to a fake "secure" IP
  via `XNetServerToInAddr` (`logon.cpp:2595-2620, 2680-2695`). Service tickets are cached on disk
  (`XONLC_TYPE_SERVICE_TICKETS`).
* With no presence service the logon finishes immediately with
  `XONLINE_S_LOGON_CONNECTION_ESTABLISHED` (`logon.cpp:2700-2712`; `ntos/net/ip.cpp:4341-4398`);
  the SG is contacted lazily by the first packet to a service.

---

## 5. Step 4 — Security Gateway key exchange and ESP data path

Code: `ntos/net/ip.cpp` — `IpXmitKeyExXbToSg` 2259-2460, `IpRecvKeyExSgToXb` 1419-1650,
`SecRegSetKey` 4032-4062, `IpXmitEsp` 1899-2025, `IpRecvEsp` 586-868, `IpRecvSecMsg` 1654-1815,
`SecRegAlloc(ipa, svc)` 3861-3880. Wire structs: `inc/xonlinep.h:2884-3170` (all `pshpack1`).
AP-REQ/AP-REP glue: `kerberos.cpp:3102-3350`, `logon.cpp:3316-3354`.

**Transport:** UDP, client port 3074 → SG `siteIPAddress`:**3074** (`ESPUDP_CLIENT_PORT`,
`ntos/net/xnp.h:391`; `ip.cpp:3876`). One security association (SA) per (siteIP, serviceID) —
the user-account XRL uses serviceID 5 but the same ticket/session key/DH X as the other services.
The client does not check that responses come from siteIP; it matches by SPI and nonce. Reply to
the observed source address:port (NAT).

### 5.1 KeyEx INIT (Xbox → SG)

UDP payload: `DWORD SPI = 0` (marks key exchange), then entries `{WORD type, WORD cbEnt, ...}`:

| entry | type | size | content |
|---|---|---|---|
| `CKeyExXbToSgInit` | 0x0102 | 24 | `dwFlags` (bit0 `XSIF_CONNECTION_SERVICE` only for presence/logon SA), `dwSpiInit` (client's receive SPI = its fake secure IP), `abNonceInit[8]`, `dwUserPerm` (0 with no users) |
| DH | 0x0110 | 4+96 | g^X |
| `KEYEX_TYPE_KERB_APREQ` | 0x0112 | 4+n | Kerberos AP-REQ |

AP-REQ: service ticket, ap-options `mutual-required`, authenticator (usage 11) with
`seq-number` 0 and `cksum = {type -131 (KERB_CHECKSUM_SHA1), 20 bytes}` where
`cksum = HMAC-SHA1(sessionKey16, InitEntry(24) || DhEntry(100))` (`ip.cpp:2384-2387`,
`kerberos.cpp:3166-3172`). `ctime/cusec` are the client's clock (cusec computed as
`(FILETIME.LowPart/10) % 1000000`, `kerberos.cpp:3446-3540` — just echo it).

**SG must:** decrypt the ticket with its own key → session key; decrypt the authenticator; verify
the checksum equals the HMAC above (this is what authenticates g^X).

### 5.2 KeyEx RESP (SG → Xbox)

Payload: `DWORD SPI = 0`, then:

| entry | type | size (exact) | content |
|---|---|---|---|
| `CKeyExSgToXbResp` | 0x0103 | **62** | `dwFlags` (**must** set `SXRF_ENCRYPT_DES`=1 or `SXRF_ENCRYPT_3DES`=2), `dwSpiInit` (echo), `dwSpiResp` (SG's SPI), `abNonceInit[8]` (echo, checked), `abNonceResp[8]`, `wXbToSgTimeoutInSecs`, `wXbToSgPulseTimeoutInSecs` (**both non-zero**: 0 makes the client drop the SA immediately, `ip.cpp:4135`), `SGADDR sgaddrInit` (20 bytes: inaSg, dwSpiSg, qwXboxID, 4 reserved), `inaInit`, `wPortInit` (client's address as seen by SG) |
| DH | 0x0110 | **100** | g^Y |
| `KEYEX_TYPE_KERB_APREP` | 0x0113 | 4+n, **must be last and fill the packet** | Kerberos AP-REP |

AP-REP enc-part (usage 12, service session key) = `EncAPRepPart{ctime, cusec` **identical** to the
authenticator's (struct memcmp, `kerberos.cpp:3310-3320`), `subkey = {keytype -131, keyvalue =
HMAC-SHA1(sessionKey, RespEntry(62) || DhEntry(100))}` (20 bytes) `}` (`kerberos.cpp:3325-3345`,
`ip.cpp:1586-1596`). No seq-number needed.

### 5.3 Diffie-Hellman and key derivation

* Group: Oakley group 1 constants, 96 bytes = 768-bit (`ntos/net/base.cpp:30-58`, `CBDHG1` in
  `ntos/net/xn.h:257`). **Byte order caveat:** `XcModExp` → `BenalohModExp` (`crypto/xcbase.c:543-555`)
  operates on little-endian DWORD arrays (word 0 least significant). The constant arrays are
  written in RFC big-endian order but fed in as-is, so the effective modulus/generator are the
  byte-reversed interpretations (`p = int.from_bytes(mod_bytes, 'little')`,
  `g = int.from_bytes(base_bytes, 'little') = 2^761`). DH agreement works regardless; the server
  must use the same convention for g^Y and g^XY, all on the wire as 96 little-endian bytes. The PC
  side debugger handshake (`xdktools/xboxdbg/secure.c:160-178`) uses the same arrays the same way.
  Verify against `BenalohModExp` before relying on this.
* `SecRegSetKey` (`ip.cpp:4032-4062`), with `K = service session key (16)`,
  `GXY = (g^Y)^X mod p` (96 bytes LE):
  ```
  for i in 0..3:
      D_i = HMAC-SHA1(K, GXY_with_byte0_plus_i || NonceInit(8) || NonceResp(8))   # GXY[0] += 1 each round
  Xbox→SG (initiator xmit):  ShaKey = (D0||D1)[0:16], DesKey = (D0||D1)[16:40]
  SG→Xbox (initiator recv):  ShaKey = (D2||D3)[0:16], DesKey = (D2||D3)[16:40]
  DES keys get odd parity (XcDESKeyParity); DES uses the first 8 bytes, 3DES all 24.
  ```

### 5.4 ESP-in-UDP data packets (both directions)

```
UDP(3074) | SPI(4, receiver's SPI) | Seq(4, big-endian, starts at 1) | IV(8) |
  DES/3DES-CBC( inner payload | pad 1,2,3.. | padLen(1) | nextHeader(1) ) | HMAC-SHA1-96(12)
```
* HMAC-SHA1 with ShaKey over SPI..end of ciphertext (incl. IV), truncated to 12 bytes.
* Encrypted length multiple of 8; UDP payload length multiple of 4; replay window 32+.
* `nextHeader`: 6 = TCP, 17 = UDP, 0 = SecMsg. The inner payload is a **bare TCP/UDP header +
  data, no inner IP header**; checksums inside ESP are not verified by the client
  (`ip.cpp:551`, `socktcp.cpp:1572`). The client addresses the service by its fake secure IP and
  `wServicePort`; the SG must terminate (or proxy) that TCP stream itself — effectively a small
  user-space TCP stack per SA.
* SecMsg types (`inc/xonlinep.h:2985-3060`): `PULSE` 0x0201 (the SG should send one or any packet
  right after RESP; the client sends a PULSE/queued SYN after RESP), `XBTOSG_PULSE` 0x0202 (client
  keep-alive), `SGTOXB_PULSE` 0x0203, `DELETE` 0x0200 (+DWORD reason).

### 5.5 Insecure-build test path (devkit libs only)

`xneto.lib` (`XNET_FEATURE_INSECURE`) supports service ID 0 with `KEYEX_TYPE_NULL_APREQ`/`NULL_APREP`
(`ip.cpp:2273-2285, 2380-2430, 1549-1569`): no Kerberos, `CAuthData` in clear, HMAC key `_abKeyNull`.
Not reachable from the normal logon path (service IDs are never 0), but a useful reference.

---

## 6. Step 5 — User-account service XRL (dashboard account creation)

Code: `online/accounts.cpp`, `online/xrlutil.cpp`, `online/upload.cpp:62-830`.
Dashboard callers: `online/xodash/XODashMain/accountdata.cpp:505-720` (`TAG_COUNT=5`,
`ALTERNATE_NAME_COUNT=5`).

**Transport:** TCP to (fake secure IP of service 5 → SG siteIP) port = `wServicePort` from PA 202
(`xrlutil.cpp:441-460, 608-650`), i.e. inside the ESP tunnel of §5. Plain HTTP/1.0.

**Request** (`xrlutil.cpp:24-27, 343-438`):
```
POST /xuacs/<name>.ashx HTTP/1.0\r\n
User-Agent: 5/1.0\r\n            (service id in hex)
Content-Length: <n>\r\n
Content-Type: xon/5\r\n
\r\n
<binary body>
```
No Host header, no auth header (authentication is the SG association).

**Response parsing** (`xrlutil.cpp:673-970`, `upload.cpp:359-470, 1135-1185`):
* Status line `HTTP...` code; any code ≥ 400 → task fails with
  `HRESULT_FROM_WIN32(ERROR_SERVICE_SPECIFIC_ERROR)` unless an `X-Err: <hex HRESULT>` header is given,
  which is returned verbatim (also on 200, if non-zero).
* `Content-Length` is required in practice: the client stops when received body ≥ Content-Length or
  the buffer is full; the server closing first → `WSAECONNRESET` error.
* Headers and body are received into the caller's (small) response buffer — keep response headers
  short (< ~400 bytes). Body is raw binary, little-endian, `#pragma pack(1)`.
* String fields are `WORD len` (LE) + UTF-8 bytes, no NUL. When parsing replies the client reads
  only the **low byte** of each length WORD (`cb = (WORD)*pby`, `accounts.cpp:484, 799, 818`).

| API | URL | request body | response body |
|---|---|---|---|
| `_XOnlineGetTags(country, maxTags=5, name=NULL)` (`accounts.cpp:307-455`) | `/xuacs/gettags.ashx` | `WORD wHeadSize=8; WORD wCountryId; WORD wMaxTags; WORD cbName; BYTE name[cbName]` (dashboard: cbName=0, 9 bytes sent incl. a 0 byte) | `WORD wTagCount; { WORD len(1..24); utf8 tag }*` (tags = "kingdoms", ≤12 wchars) |
| `_XOnlineReserveName(nick, kingdom, maxNames=5, country)` (`accounts.cpp:618-865`) | `/xuacs/reservename.ashx` | 45 bytes: `ULONGLONG qwMachineId; WORD wCountryId; WORD wMaxNames; WORD len; utf8 nick; WORD len; utf8 kingdom;` rest uninitialized | `WORD wNameCount; { WORD len(1..16); utf8 nick; WORD len(1..12); utf8 kingdom }*` (0 names = accepted as typed? — dashboard decides; suggestions otherwise) |
| `_XOnlineCreateAccount(USER_ACCOUNT_INFO*)` (`accounts.cpp:959-1240`) | `/xuacs/createacct.ashx` | `BYTE bCountryId; BYTE bCardTypeId; FILETIME ftCardExpiration;` then strings: nick, kingdom, **PIN**, firstName, **firstName again**, lastName, street1-3, city, state, postal, phonePrefix, phoneNumber, phoneExt, cardHolder, cardNumber, email, bankCode, branchCode, checkDigits. Sent length = `3*sizeof(USER_ACCOUNT_INFO)` (~6.8 KB) regardless of content — trailing bytes are garbage | fixed 68 bytes: `ULONGLONG qwUserID; BYTE key[16]; CHAR domain[20]; CHAR realm[24]` |
| `_XOnlineAccountTempCreate` (dev tools) | `/xuacs/tempcreateacct.ashx` | raw `XONLINE_USER` | raw `XONLINE_USER` |
| `XOnlineGetLockoutInfo` | `/xuacs/getlockoutinfo.ashx` | see `accounts.cpp:1330-1480` | — |

CreateAccount quirks (`accounts.cpp:1011-1027`): PIN is `WORD n` then the PIN bytes are copied
2 bytes too far (`memcpy(pby + sizeof(WORD), …)`), so the server sees `n` bytes of which the first
two are uninitialized and the last two get overwritten by the next field's length — treat the PIN
field as unreliable. The client **ignores the returned realm** and hard-codes `"passport.net"`
(`accounts.cpp:1054`), uses `name`/`kingdom` it sent, and the returned `qwUserID`, `domain`, `key`;
then writes the user to the HDD (`_XOnlineAddUserToHD`). That key is what the server must later
accept in the user's AS-REQ (`cname = nick.kingdom@domain`, realm `passport.net`, sname
`krbtgt/passport.net` at `passport.as.xbox.com`).

User identity on the server side: the real service learned the caller from the SG
(`SGMSG_TYPE_AUTHDATA_REQ/REP`, `inc/xonlinep.h:3073-3105`). A combined SG+HTTP server can map the
TCP connection to its SA and the ticket's auth-data; `reservename` also carries `qwMachineId` in
the body.

---

## 7. Secrets and blockers

### 7.1 Per-console online key (MACS) — the only key the server cannot invent
* Real client (EEPROM path, `logon.cpp:763-794`): serial = EEPROM factory serial
  (`XC_FACTORY_SERIAL_NUMBER`), K = EEPROM `OnlineKey` (`XC_FACTORY_ONLINE_KEY` = value index
  **0x102**; factory section offset 24, i.e. EEPROM byte 0x48) run through
  `MorphKeyByHardDriveKeyEx` = RC4 keyed with `HMAC-SHA1(HDMORPH_RAND_KEY, HDKey)` (`inc/cryptkeys.h:149-170`;
  `HDMORPH_RAND_KEY` = `60 59 E8 2E DF BF 7F D3 23 35 74 2A 64 8B B1 2C`). HDKey is in the
  encrypted EEPROM section.
* Microsoft learned each console's online key at the factory only as an RSA-encrypted blob
  (`ONLINE_KEY_PKENC_SIZE 128`, `XBOX_ENCRYPTED_ONLINE_DATA`, `inc/cryptkeys.h:27,62-75`) — the
  private key was never in the box or the leak. **A server cannot derive K from the serial.**
* Ways around it, none needing a client patch:
  1. Leak-built `xonline.lib` (dashboard here): K is the public devkit constant — no blocker.
  2. Emulator / owned console: read EEPROM + HDD key, compute K, and register it with the server
     (or send it out-of-band).
  3. Skip MACS entirely: pre-seed HDD config sector 1 (`partition0`, sector
     `XBOX_CONFIG_SECTOR_INDEX+1`, `ntos/net/xnetp.cpp:46-110`) with an
     `XC_ONLINE_MACHINE_ACCOUNT_STRUCT` (`inc/xonlinep.h:1708-1722`) whose key is server-issued,
     encrypted with `EncryptKeyWithHardDriveKeyEx` (3DES via `MorphKeyWithKey`, constants in
     `inc/cryptkeys.h:176-221` (MorphKeyWithKey at 185)) and signed with HMAC-SHA1 key `"Mar/2002 SDK"` incl. NUL
     (`ONLINE_USER_SIGNATURE_KEY`, `online/xonp.h:678, 714-731`). All constants are in the leak.
     The check is `VerifyOnlineUserSignature` at `logon.cpp:1167`. (HLE alternative: hook
     `GetMachineAccount`, `logon.cpp:1125`.)
  4. Patch/HLE: the MACS key is used only in `XkerbBuildAsRequest`/`XkerbVerifyAsReply` for the
     MACS call (`logon.cpp:800-821`); forcing the `#else` devkit branch or substituting K there
     also works.

### 7.2 Everything else is symmetric and server-chosen
* No RSA/public-key verification anywhere in logon, Kerberos, SG key exchange, or XRL account
  calls. The client authenticates servers only through keys the server itself issued (machine
  key, TGT/service session keys) or the online key. No "server signature" field is checked
  (`XKERB_AD_XBOX_WITH_SIGNATURE.ServerSignature` is SG↔service internal and unused by the client).
* The DH is unauthenticated except via the Kerberos session-key HMACs above, which the replacement
  KDC/SG controls.
* Public-key checks exist only in content/autoupdate paths, not needed for logon or accounts:
  `XcVerifyPKCS1Signature` in `online/contdl.cpp:261`, referral public key in
  `online/contutil.cpp:514` — relevant later for content download / dash autoupdate.

### 7.3 Implementation gotchas (non-secret)
* Exact-length checks: PA 202 cipher = 212, PA 203 cipher = 120, KeyEx RESP entry = 62, DH entry
  = 100, AP-REP entry must end the packet.
* SXRF DES/3DES flag mandatory; SG timeouts non-zero.
* DH byte order (little-endian bignum), see §5.3.
* UDP-only Kerberos, ≤1460-byte replies, ticket ≤1024 bytes.
* TGS-REP must list every requested service with S_OK + port; S_FALSE triggers a re-request
  (and a client indexing bug at `logon.cpp:2554` uses `i` instead of `j`).
* HTTP: send Content-Length; keep headers small; non-200 needs `X-Err` to give a useful HRESULT.
* Disk caches (combined TGT, service tickets, machine account) persist across boots; changing
  server keys requires the server to accept old tickets or the cache entries to expire/be wiped.

### 7.4 Server-side code in the leak
* **No KDC, MACS, SG or xuacs server source is present.** Only clients/test harnesses:
  * `online/test/xkdctest/` — PC-side tester that speaks to MACS/XKDC using the same client
    Kerberos code (`xkdctestkerb.cpp`, `xktxmacscreate.cpp`, `xktxonlinelogon.cpp`); its `.ini`
    files show the test machine namespace (`dev=999999999999,PASSWORD`, serial/online-key pairs,
    expected errors such as `STATUS_WRONG_PASSWORD` for unknown serials). Good for building a
    regression client against a new server.
  * `online/test/gssserver/` — generic Windows SSPI/GSS sample server, not Xbox-specific.
  * `test/online/server/{SGBVT,XSGtest,...}` — SG test clients/load tools.
  * `online/tools/PwdToKey` + `online/md4ms.cpp:291` — test-account key = MD4(UTF-16LE password)
    (NT hash), i.e. ordinary RC4-HMAC password keys.
  * `online/tools/OnlineAccounts` — devkit tool that writes the devkit serial/online key into the
    EEPROM (`OnlineAccounts.cpp:247-320`) and (disabled) machine-account injection
    (`ModifyMachineAccount`, line 122).
  * `online/service.cpp:35-260` — insecure-build `xonline.ini` parser (`S<id>:ip:port`, `MKDC`,
    `SG`, retry tweaks). In this tree the parsed overrides are **not consumed** by `logon.cpp`
    (only retry counts and `FORCEMKDC` take effect), so DNS is the redirection mechanism.

---

## 8. Minimum server for the dashboard (ordered implementation list)

1. **DNS**: answer A records for `macs.xbox.com`, `passport.as.xbox.com`, `kdc.xbox.com` (and
   optionally `passport.web.xbox.com`, `passport.tgs.xbox.com`) with your server IP. Point the
   box's DNS (EEPROM `OnlineDnsAddress` / DHCP) at it.
2. **RC4-HMAC + Kerberos ASN.1 core**: etype 23 encrypt/decrypt with raw usages (§2), Knonce
   derivation, AS-REQ/TGS-REQ/AP-REQ decode, AS-REP/TGS-REP/AP-REP/KRB-ERROR encode. Return
   `KRB_AP_ERR_SKEW` with `stime` to fix the client clock.
3. **MACS on UDP 88** (dispatch by `realm == macs.xbox.com`): look up K for the serial (devkit
   constant for the leak build), verify PA-ENC-TIMESTAMP, mint `{qwUserID, name, realm, key}`,
   reply AS-REP + PA 203. Persist the machine account. (Or skip by pre-seeding config sector 1.)
4. **AS for `krbtgt/xbox.com`** (same UDP 88 socket, realm = machine realm): verify timestamp with
   the machine key, issue TGT (encrypted with your TGS key), session key etype 23.
5. **TGS for `sg/S<id>`**: decrypt TGT from PA-TGS-REQ, check authenticator (+ optional MD5 body
   checksum), decrypt PA 201, reply TGS-REP with a service ticket (your SG key; embed XboxID,
   service list, title in auth-data), and PA 202 = `{hr=S_OK, siteIP=<SG IP>, services {2,4,5}
   each S_OK + port, e.g. 80}`.
6. **SG on UDP 3074**: parse KeyEx INIT, verify AP-REQ + HMAC checksum, generate Y, NonceResp,
   SPI; send RESP with 3DES (or DES) flag, non-zero timeouts, AP-REP with SHA1 subkey; derive ESP
   keys; implement ESP encap/decap, replay window, SecMsg PULSE/DELETE handling.
7. **Inner TCP endpoint per SA**: minimal TCP (SYN/ACK, in-order data, FIN/RST) bound to each
   service port, or a userspace-TCP → real socket proxy to a local HTTP backend.
8. **xuacs HTTP handlers**: `gettags.ashx`, `reservename.ashx`, `createacct.ashx` (binary formats
   in §6), returning `HTTP/1.0 200` + `Content-Length`, `X-Err` on errors. Store created users
   `{nick.kingdom@domain, realm passport.net, key, XUID}`.
9. **User logon (next step after account creation)**: AS for `krbtgt/passport.net` with the user
   key, compound-identity machine AS, TGS including presence (1) — which then requires the SG
   "connection service" flag, SGTOXB/XBTOSG pulse handling and a presence/notification service.
   Not needed for steps 1-5.
10. Billing offering (service 4) and message (service 2) only need to appear in PA 202 for the
    machine logon; implement their endpoints when the dashboard starts using them.

---

## 11. Retail 5849 — live findings (2026-10-20, branch claude/project-thread-gudfcx)

The retail Live dashboard (`xonlinedash.xbe` from the 5960 OS) differs from the leak
trunk above in several wire details. All confirmed live in the cloud against
`tools/liveserver`, driving the real retail dash headless under xbcompat.

**Key exchange entry numbers** (vs leak 0x010x/0x011x): INIT `0x5802`, g^X/g^Y `0x5880`,
AP-REQ `0x5882`, AP-REP `0x5883`, padding entry `0x5886` (ignored). RESP `0x5803`.

**Diffie-Hellman**: Oakley group 1 with generator = integer **2** and modulus = the real
RFC-2409 prime in natural big-endian form. (The leak's `base.cpp` little-endian byte arrays
give a different gen/modulus the retail console does NOT use — verified from the console's
own `XcModExp` inputs.)

**RESP address tail**: the 32-byte SGADDR+addr tail is 3DES-CBC encrypted with the server's
transmit DES key, IV = NonceResp (beta leaves it clear). Responder SPI low byte must be 0,
upper 3 bytes non-zero.

**Key derivation**: five HMAC-SHA1(skey, GXY||NonceInit||NonceResp) with GXY[0]++ each round
(beta: four). d[0:16]=sha, d[16:40]/d[56:80]=the two 3DES groups, d[40:56], and the fifth
round's d[80:96] = the per-direction IV seeds (console xmit seed d[80:88], recv seed d[88:96]);
the IV seeds never travel on the wire.

**ESP framing (retail, both directions)**: `spi(4 LE) + cipher(8n) + seq(2 LE) + HMAC[0:10]`.
- No ESP pad/pad-length/next-header trailer. The **inner protocol is the low byte of the SPI**:
  bits 0-4 = protocol (0 = secmsg; 1/4/7/10 = compressed TCP/IP variants), bits 5-7 encode the
  compressed IP-header variant/count. The console's own SPI and ours both carry low byte 0 as
  the base; each packet ORs the control byte in.
- HMAC-SHA1 over `seq(4 LE) + spi(4, as on the wire incl. control byte) + cipher`, first 10 bytes.
- **Per-packet IV** from the 8-byte seed and the sequence counter ctr:
  `s0,s1 = seed[0:4],seed[4:8] (LE); d0 = (s1*ctr).hi ^ (s0*ctr).lo; d1 = (s1*ctr).lo ^ (s0*ctr).hi;
  IV = pack('<II', d0, d1)`. ctr==1 yields the seed itself. (Console routine 0x1346d3.)
- secmsg payloads are secmsg entries zero-padded up to the DES block; the parser stops at a
  zero wType.

**Secmsg types (retail, 0x53xx base; layouts per inc/xonlinep.h)**: pulse `0x5300`
(`{wType, cbEnt=8, dwSeqAck}`, used both directions — XbToSg and SgToXb), bare keepalive `0x5301`
(`{wType, cbEnt=4}`, console sends per-SA, ignored on receipt), list/secreg `0x5302`/`0x5309`.
Recv dispatch in xnet at `cmp $0x5300`=0x1387d0, `cmp $0x5301`=0x138809; the 0x5300 handler
stores dwSeqAck and marks the SG alive (0xd0c). ESP recv demux at 0x138a45+, secmsg parser
0x1387a4, transmit 0x135b..; proto/control byte read at 0x138b73 (`al&0x1f`, `al>>5`).

**Verified this session**: full retail keyex accepted; encrypted ESP works both ways; the server
sends SgToXb pulse acks that the console accepts (probe hit at 0x1387a4 secmsg-parser and
0x1387fe 0x5300-store). The dashboard reaches the **XBOX LIVE account menu**
(ACCOUNT MANAGEMENT / NEW ACCOUNT / NETWORK SETUP) and navigates New Account and Account Recovery.

**Still open (the account-service layer)**:
- **Compressed TCP/IP reconstruction** (proto 1/4/7/10): the console packs the IP/TCP header into
  the control byte + SA state rather than sending a full header. Must be decoded from the recv
  demux (0x138b73→0x138e97) and transmit (0x135b..) to carry TCP both ways. No TCP captured yet
  because the dashboard gates service contact behind on-screen text entry.
- **Account UI needs text entry before any network**: New Account's "subscribe/CONTINUE" fails
  locally with "Unable to continue due to technical difficulties" and emits ZERO DNS/TCP — a
  local precondition (logon state / clock / offering backend), not the SG link. Account Recovery
  requires GAMERTAG + CARDHOLDER/CREDIT CARD/PHONE/POSTAL before CONTINUE contacts the service.
- **XUACS/presence HTTP surface** (from the dash image): `/xuacs/{EnumerateEligible,createuseracct,
  createowneracct,restoreaccount,reservename,accepttou,verifyvoucher,getsubscriptionstatus,...}.ashx`,
  `/xpnfront/{xpresence,xnotification,xmessaging,xteams}.srf`, `/xbos/Offering*.ashx`, `/xsig/GetSigningKey.ashx`.
- **Pragmatic route to a *visible* sign-in**: pre-provision an account blob on the HDD so the dash
  auto-signs-in a gamertag (sidesteps the text-entry UI), while the transactional XUACS backend is
  built out separately.

Cloud harness: `/tmp/retail/` — `go.sh NAME SECS` (restart liveserver + run dash headless),
pad over `/tmp/retail/pad` (`echo "down a" > pad`), screenshot by `touch /tmp/retail/NAME.req`,
`LIVE_DEBUG=1` + `LIVESERVER_DUMP` for wire logs, `PROBE=addr,...` for guest register probes.

### 11.1 Connection-test finding (2026-10-20)

NETWORK SETUP → CONNECT (the "Test Xbox Live Connection", no text entry) does:
DNS as./tgs.xboxlive.com → Kerberos → SG keyex → one 0x5301 ping → then after ~20s
shows **"COMMUNICATION ERROR — Could not connect to Xbox Live"**. The console sends NO
service TCP during the test (only the keyex + ping reach the gateway).

In the leak trunk the machine logon reaches XN_LOGON_STATE_ONLINE the instant the console
processes a valid KeyEx RESP (ip.cpp:1631-1640, `IpRecvKeyExSgToXbResp`), then it sends
SECMSG_TYPE_PULSE (the 0x5301 we see); no service round-trip is needed for logon itself.
So the retail connection test (and New Account "technical difficulties") fail on a check
the retail 5849 build adds *beyond* this older trunk — it is NOT the SG link (verified up)
and NOT a service TCP it tries and we drop (nothing is sent). Leads for next session:
- The logon keyex INIT for `_pSecRegLogon` sets `XSIF_CONNECTION_SERVICE` (0x1) in `_dwFlags`
  (ip.cpp:2347-2349). Check whether the RETAIL console sets it and whether the gateway must
  acknowledge/provide the connection service (queue-flags push via the SgToXb pulse change
  list: control byte 0x04 = SGPULSE_QFLAGS_CHANGE, then dwQFlags + dwSeqQFlags) before the
  console treats Live as reachable. `IpLogonSetQFlags`/`IpLogonGetQFlags` (xnp.h:1632-1633).
- Verify the RESP timeout fields (`_wXbToSgPulseTimeoutInSecs`, `_wXbToSgTimeoutInSecs`) and
  whether a too-short value makes the retail test give up.
- The retail connection test may require an actual service reachability probe the older
  trunk doesn't; re-check against the 5849 xnet disasm (the logon-state machine).

### 11.2 Retail post-keyex stack, traced live in xbcompat (2026-10-20)

The §11.1 "leads" are superseded by a live trace of xonlinedash's own xnet (addresses
below are in xonlinedash.xbe). CXnIp is the XNet singleton; its key offsets: `+0xaf0`
`_uiLogonState`, `+0xaf4` `_pSecRegLogon`, `+0xa94` service-list head, `+0xd0c` last SgToXb
dwSeqAck (logon only). Per-SA SecReg: `+0x2` state byte, `+0x0` flags, `+0x44` nonce_init.

**Boot/CONNECT traffic is the machine SA, not a user logon.** Every keyex INIT has
`_dwFlags == 0` (no `XSIF_CONNECTION_SERVICE`); account `SN...@machines.xbox.com`, services
[4,5,11,20]. The RESP handler (0x138002, dispatched for entry 0x5803 at 0x1386fa) fully
validates (g^Y 0x5880 len 0x64, AP-REP 0x5883 last, Kerberos verify, key-derive, address
tail) and sets SecReg state 5. Only *if* `SecReg == _pSecRegLogon` (check at 0x13823a) does
it look for entry **0x5887** (authdata) and set `_uiLogonState = 3` (online) / `6` (offline).
For the machine SA that branch is skipped (jne 0x1382e4) — so **0x5887 is only for a real
user logon**, not the connection test. Correction to earlier notes: 0x5887 is NOT the
connection-test blocker.

**Heartbeat (FIXED in sg.py, commit 10c7a427).** Directions, from the recv dispatch 0x1387a4:
`0x5300` = SgToXb pulse (gateway→console, {wType,cbEnt=8,dwSeqAck}); `0x5301` = XbToSg pulse
(console→gateway, cbEnt=4, no body — ack the ESP seq). The console pulses with 0x5301 and
waits for a 0x5300 ack. Two bugs: the server answered an inbound 0x5300 (never sent) and
ignored 0x5301; and it sent an unsolicited 0x5300 right after keyex while the SecReg was in
state 5, which the ESP inner dispatcher (0x138dfc: `state==5` → `_uiLogonState=6`, error)
treats as an error. Fix: drop the premature pulse; answer 0x5301 with 0x5300(dwSeqAck=ESP
seq). After the fix the console advances and sends the service-connect request.

**Service layer (secmsg 0x53xx), dispatch 0x1387a4 (called from 0x138e97, only when the
reconstructed inner proto `ctrl&0x1f == 0`):** `0x5307` XbToSg service-connect/poll
(console→gateway, built 0x13b392: 8-byte service id, 16-bit index, counter, flag);
`0x5308` SgToXb service reply (gateway→console, parsed 0x13c061: match id+index in the
CXnIp+0xa94 list, record must be state 4/5 on this SA, then flag-bit-2 = control copy to
record+0x9c else windowed data stream keyed on a monotonic counter 0x13a931/0x14055e);
`0x5304` re-key (→0x13869c), `0x5306` disconnect, `0x5309` presence/user change, `0x530c`
state ack. On CONNECT the console opens ONE service (0x5307) and retransmits until it gets a
0x5308; no 0x5308 → COMMUNICATION ERROR.

**Why our 0x5308 is dropped (the current blocker).** secmsgs ride as UDP datagrams (port
0x20c) inside Xbox Live's state-based IP/UDP **header compression**. On receive the console
reconstructs a full IP packet, then: IP input 0x1390bf (parses IHL) / reassembly 0x139294 →
by IP proto: UDP 17 → 0x138f99 (checks the BE length word at +4 == packet length, port ==
0x20c or CXnIp+0x8ce) → 0x138a06 (strips a 12-byte header, `ctrl&0x1f` at 0x138b73 picks
proto; proto 0 → 0x1387a4). The control byte is built from the connection's compression
STATE (0x135140: state 5 → variant 4 = ctrl 0x80 (the pulse's value); 0xc/0xd/0xe → 8; else
2/0x30 with a 0x20 bit) and selects which header fields are elided. **Verified empirically:
our 8-byte pulse (ctrl 0) reaches 0x1387a4, but the 24-byte 0x5308 (ctrl 0) does not** — a
variable-size reply needs the matching compressed framing (or an accepted uncompressed
variant), which the server does not yet emit. sg.py now has a 0x5308 scaffold (commit
168be450) that is correct in content but not yet accepted for this reason.

Remaining to a visible sign-in: (1) emit the compressed IP/UDP framing so 0x5308 reaches
0x13c061; (2) the service transport (windowed 0x5307/0x5308 data); (3) the account HTTP
(XUACS/xpnfront) carried over compressed TCP (IP proto 6 → 0x1348fa); (4) for a real user
logon, the 0x5887 authdata block in the logon RESP. Cloud harness unchanged (/tmp/retail,
go.sh NAME SECS, PROBE=addr,..., LIVESERVER_DUMP). des.c/misc.c debug reverted before commits.

### 11.3 Retail 5849 sign-in and account screens working (2026-10-20, supersedes the open items in 11.2)

The cloud harness now signs a gamertag in to the retail Live dashboard against tools/liveserver
and walks the Account Management screens. Corrections to 11.2 first:

- **secmsg 0x5300 is DELETE, not a pulse.** 0x5301 is the bare pulse. QoS: 0x5307 probe, 0x5308 reply, one reply per probe packet.
- **Compressed IP/TCP is done** (sg.py `send_retail`/`esp_retail`): the control byte says which header bytes were elided; the kept header bytes are split around the DES-CBC cipher. HMAC-SHA1 covers clear + seq32 + spi, padded, + cipher; 10 bytes kept.
- **SgToXbResp encrypted tail** = {SGADDR (20), ina, port, wXbToSgTimeoutInSecs, wXbToSgPulseTimeoutInSecs}. Use (900, 30). With them swapped the console sends DELETE after 30 s idle.

**Accounts on the hard disk** (xonlinedash 0x1197c0; tools/liveserver/mkuser.py writes them): config sector 3 of partition 0 is a directory of eight 12-byte {DWORD in use, iv[8]}; account i is a 108-byte record at the start of config sector 4+i: {qwUserID, dwUserFlags, name[16], dwUserOptions, passcode[4], domain[20], realm[24], key[16] (3DES-CBC under MorphKeyWithKey(hdd key), the entry's iv), dwSignatureTime, signature[8] = HMAC-SHA1(hdd key, first 100 bytes with the key in clear)[0:8]}. dwUserFlags in the record must be 0 (nonzero hides the account).

**User logon = chained AS-REQs.** Each user's AS-REQ comes first, then the machine's AS-REQ carries PA 130 (KERB_PA_COMPOUND_IDENTITY, SEQUENCE OF Ticket) with the TGTs so far. Its timestamp and reply are keyed with compound_key(client key, previous TGT session key) = HMAC-MD5(HMAC-MD5(key, "signaturekey\0"), MD5(pack('<I', 1024) + session key)). Other PA types seen: 131 PAC_REQUEST_EX, 204 (48 bytes, unknown), 206 signed client version. Tickets are cached on the HDD, so later runs skip the KDC.

**Country.** The dashboard reads the user's dwUserFlags bits 8-15 (XONLINE_USER_COUNTRY_MASK; US = 103), which come from the TGS service-address reply's dwUserFlags[4] (one per logon slot), and selects the Country object whose LocaleCode matches (0x59b4c).

**Content downloads** (0x5b79b; service 11; plain HTTP GET, any 200 with Content-Length):

| Codes | Path |
|---|---|
| 0x1d / 0x21 / 0x25 | /Country/<NoRegion\|America\|Asia\|Europe>/CountryMaster2.INX |
| 0x1f / 0x23 / 0x27 | /<CountryName>/CountryInfo2.INX |
| 0x17 / 0x18 / 0x19 | /<locale>/TermsOfUse.ini, CodeOfConduct.ini, PrivacyStatement.ini (seen as /US/TermsOfUse.ini) |
| 0x1c | /HeidiDashLocalization/<english.inf…> |
| 0x2a | RefundInfo.ini |

INX (parser 0xe03fb): UTF-16LE with a BOM, `[Section]` headers, `Key="value"` (values MUST be quoted or the whole file fails), `#`/`;` comments. DayPosition/MonthPosition/YearPosition are 0-based (the formatter 0x588c0 prints nothing past 2). The section is the country name (used in the CountryInfo2 path); setter 0x59388 keys: LocaleCode, DayPosition, MonthPosition, YearPosition, DateSeparator, DateOfBirthPresent, MinimumAge, ParentAge, PostalLookup, PostalLookupRightJustify, StateSceneTitleLocTag, BillingField; keys with a '.' are billing field definitions (PostalCode., PhoneNumber., CreditCardNumber., CreditCardType., StateListItem.).

**Presence check-in** (needed, else logon fails 0x80151001 "cannot access service"): one message per POST to /xpnfront/xpresence.srf (Content-type xon/1), no HELLO. BASE_MSG_HEADER {type, len, seq, SGADDR[20]}; the check-in is type 0x401 (built 0x11ee71) {qwUserID, …XNADDR, keys, gamertag}. Reply parser 0x11efc8 (no type check): total must equal 0x20 + dwMsgLen, at least 0x3e; qwUserID at 0x20, WORD 0x28, HRESULT 0x2a, WORD cItems 0x34, DWORD cTitles 0x3a, titles[], items.

**Account service** (/xuacs/*.ashx service 5, /xbos/*.ashx service 4 or 9; binary POST bodies, reply = HTTP body; an empty 200 reads as zeros). Table at xonlinedash 0x19eb50, 20-byte entries {service, 0, url, 0, 0}.

| URL | Builder / reader | Request / reply |
|---|---|---|
| reservename | 0x121dbc / 0x120b71 | {qwMachineID, BYTE country, WORD wMaxNames, ustr name} / {WORD n, n × ustr (≤16)}: n = 0 means free and reserved; names are suggestions for a taken one (dash check 0x5d92d) |
| createowneracct | 0x12248a / 0x121f40 | {qwMachineID, …header to 0x22, ustr gamertag, 18 billing ustrs} / {qwUserID, key[16], domain[20], realm[24]}; the dash writes the account to the HDD itself |
| getpaymentinfo | 0x120dfb / 0x120e33 | {qwUserID, DWORD} / {BYTE country, BYTE language, DWORD, DWORD, BYTE, BYTE, 17 ustrs: first, last, street×3, city, state, postal, phone prefix/number/ext, e-mail, card number, holder, 3 bank} |
| verifybillingpin | 0x1216ad | {qwUserID, digits[4]} / empty 200 accepts |
| getsubscriptionstatus | 0x122214 / 0x121a9c | {qwUserID, qwMachineID, dwTitleID, 1, 1} / {qwOfferID, BYTE status, FILETIME start, FILETIME end, 5 bytes, BYTE renewal (0 none, 1 voucher, 2 normal), qwRenewalOfferID} + price(current) [+ price(renewal)] |
| verifyvouchergetoffer | 0x121b61 / 0x121c0b | {BYTE, ustr code} / {qwOfferID, …} |
| /xbos/OfferingDetails | 0x11a6e9 / 0x119fef | {qwUserID, qwMachineID, dwTitleID, qwOfferID, DWORD, WORD lang} / {DWORD size, price, DWORD cbBlob, blob = INX [SubInfo] VERSION="1" TITLE= DATA=} |

ustr = {WORD length, UTF-8}. price = {DWORD whole, BYTE fraction, BYTE currency format, char ISO[3], DWORD flags: bit 0 costs something, bits 4-7 frequency in months, bits 16-18 tax kind}. Status byte: 0 pending, 1 enabled, 2 suspended, 3 expired, 4 canceled, 0x80 expired without violations.

Other table entries (formats not traced): verifynickname (/xbos), getlockoutinfo, createuseracct, setpaymentinfo, setusersettings, setuserpin, verifyvoucher, restoreaccount, changegamertag, setuseraccountstatus, accepttou, troubleshootaccount, clearsubscriptions, verifyvouchergetoffer, EnumerateEligible, /xbos/OfferingCancel, /xbos/OfferingDetailsNoUser.

Harness: /tmp/retail (go.sh NAME SECS; nav.sh NAME SECS "pad|secs|shot"...; SIGNIN=0 skips the sign-in). Microsoft retail files stay in-session.

### 11.4 Retail 5849 New Account flow (2026-10-20)

The cloud harness walks New Account end to end against tools/liveserver. Screen order and what each one needs from the server:

1. Welcome, Billing Region (CountryMaster2.INX), Terms of Use (TermsOfUse.ini).
2. Xbox Live ID: gamertag keyboard, then date of birth. `reservename` with an empty list = name free and reserved.
3. Activation (subscription code keyboard): `verifyvoucher`, then `verifyvouchergetoffer` (reply = qwOfferID), then `/xbos/OfferingDetailsNoUser` (same reply as OfferingDetails).
4. Purchase Details: text comes from the SubInfo blob key **PURCHASE_DETAILS** (not DATA; DATA feeds Account Management). The dash replaces the tokens `[NAME] [PRICE] [FREQUENCY] [TAX_STRING]` itself (bracket replacer 0x4dd2f; NoUser consumer callback 0x5e275).
5. Refunds (RefundInfo.ini).
6. Credit Card: Payment Method list, Cardholder Name (keyboard), Credit Card Number (keypad), Expiration Date (month/year editor), Phone Number (keypad).
7. Billing Address: Street, City (keyboard), State (list), Postal Code (keypad).
8. "Microsoft" and "Partners" information-sharing questions (YES / middle / NO; focus starts on the middle).
9. Create Account? (shows the gamertag; focus starts on NO, which asks to quit and lose progress): YES sends `createowneracct` (171-byte body: header, gamertag, then the billing ustrs in getpaymentinfo's order: first and last name (from the cardholder name), street ×3, city, state, postal code, phone prefix/number/extension, cardholder, card number, ..., and last the subscription code).
10. Congratulations, "You have signed up as" the gamertag. The dashboard has already written the account to the hard disk and signed it in (user AS-REQ, machine AS-REQ, TGS, presence check-in); ACCOUNT SETTINGS and PASS CODE work from there.

**Billing fields come from CountryInfo2.INX.** Without them the Payment Method list is empty. Keys (all values quoted):

```
BillingField="BillingStreet_1,BIL_STREET_ADDRESS,Alpha,1"   ; Field,LOCTAG,Keyboard,required
BillingField="BillingCity,BIL_CITY,Alpha,1"
BillingField="BillingState,BIL_STATE,State,1"
BillingField="BillingPostal,BIL_ZIP,10Key,1"
CreditCardType.CardType="VISA"            ; also MASTERCARD, AMERICAN_EXPRESS
CreditCardNumber.Min="13"  CreditCardNumber.Max="16"
PhoneNumber.Min="10"  PhoneNumber.Max="10"
PostalCode.Min="5"  PostalCode.Max="5"     ; .Formula optional
StateListItem.State="CA,California"
```

Account task handler: 0x5f9c8(case, &hr), jump table 0x602fd; case 0x0d verifyvoucher, 0x28 getoffer, 0x29 OfferingDetailsNoUser, 0x17 ToU download, 0x1d CountryMaster2, 0x1f CountryInfo2. Returning 0x82000003 means "continue", 0 means done.

Numeric keypad layout: rows `1 2 3 BACKSPACE` / `4 5 6 < >` / `7 8 9 DONE` / `0`, focus starts on 1.

**Known xbcompat bug: occasional "Unable to continue due to technical difficulties".** Sometimes a download or account request completes with 0 bytes and the dash maps it to WSAECONNRESET (xonline 0x1207c3; leak `online/download.cpp` treats a zero-byte read as a reset). The server sent the whole reply: traces of failing and passing runs are identical on the wire. TCP had the data buffered, but the FIN completed the pending recv with xfercnt 0 (`ntos/net/socktcp.cpp`: FIN → SockReqComplete with no copy), so it looks like TcpRead racing the receive DPC in xbcompat's emulation. dispatch_lock was reviewed without finding the hole. Workaround: press OK, then Continue, which retries.

## 12. Insignia instead of the test server (2026-10-21)

sokky asked for XBOX LIVE on the Sion to use Insignia (insignia.live, the public Live revival). What it takes, as found from here:

* **Registration** is one HTTPS request, the one the Register button of Insignia's Setup Assistant (nxdk homebrew, default.xbe) sends: `POST https://insignia.live/api/v1/xbox`, `Content-Type: application/json`, body `{"serial_number": "<12 digits>","hdd_key": "<32 hex, upper case>","online_key": "<32 hex, upper case>"}`. Serial = EEPROM factory value 0x100, online key = factory value 0x102 (raw), hdd key = XboxHDKey. Replies: 200 registered, 422 refused (JSON message, e.g. serial taken), 429 rate limit. With hdd and online key the server can work out the MACS key (online key morphed by the hdd key, §7.1). The assistant first checks the serial is 12 digits, the hdd key isn't all zero, and the EEPROM's decrypted hdd key matches XboxHDKey. Under xbcompat it crashes at its first VGA DAC write (0xfd6813c8), so `tools/liveserver/insignia.py register <hdd>/console-id` sends the request instead.
* **DNS**: insigniaDNS answers only the names in `https://insignia.live/dns_zones.json` (2026-10: macs/as/tgs.xboxlive.com -> macs/as/tgs.insig.uk = 49.13.57.101, xds.xboxlive.com -> xexds.xboxlive.com) and forwards the rest. `liveserver.py --insignia` does the same (built-in copy of the list when it can't fetch it; the Sion's trimmed Python has no urllib.request).
* **Accounts**: invite code from insignia.live by email; New Account with that code, card 4111 1111 1111 1111, the billing email must be the invite's.
* **Cloud limit**: this container has no outbound UDP (Kerberos 88, SG 3074) and intercepts every UDP 53 query (any address answers like a normal resolver, so a "DNS works" test from here proves nothing). Insignia sign-in can only be tried on the Sion. The cloud dash run with `--insignia` DNS sends its MACS AS-REQ (455 bytes) to 49.13.57.101:88 and shows "A network connection could not be created" when nothing answers.
* **Sion**: launch-title reads `/data/xbox-live/service` (`insignia` or local) and swaps the hard disk's Live accounts per service: config sectors 1 (machine account) and 3-11 (users) of partition0.img plus `E:\CACHE` (LocalCacheNN.bin ticket caches, `online/localcache.cpp`) go to `<hdd>/live-accounts/<service>/`. Kit: bootani/sion/xbox/push-insignia.
* **Games online (2026-10-21)**: launch-title also gives every game the network card (`XBCOMPAT_NET=nat`, `XBCOMPAT_DNS=127.0.0.1:5353` through the same server) unless `/data/xbox-live/games-net` says `off`. xbrun.py sets XBCOMPAT_LAUNCHER to itself, so later XLaunchNewImage hops skip launch-title but keep the env.
