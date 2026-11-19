# ReadBookY for CrossPoint X4 Pro — Complete Integration Specification

**Target hardware:** XTEINK X4 Pro  
**Firmware base:** CrossPoint 2.x fork  
**PlatformIO environment:** `x4pro`  
**ReadBookY stack:** Vanilla JS, Firebase Authentication, Firestore, Firebase Storage, Cloud Functions, Page backend  
**Goal:** Make the XTEINK X4 Pro behave as a first-class ReadBookY device while preserving normal CrossPoint functionality.

---

## 1. Product Goal

Implement the complete ReadBookY integration in one development effort.

The finished X4 Pro should support:

```text
CrossPoint
│
├── normal EPUB reading
├── offline StarDict dictionary
│
└── ReadBookY
    ├── account login
    ├── ReadBookY library
    ├── EPUB downloads
    ├── reading-position sync
    ├── trusted-reading-position sync
    ├── highlights
    ├── private/public highlights
    ├── comments
    ├── replies
    ├── reactions
    ├── offline mutation queue
    ├── two-way synchronization
    ├── Ask Page
    ├── Page history
    └── offline Page question queue
```

Do not attempt to run Firebase JS, EPUB.js, or Gemini on the X4 Pro.

The ESP32-S3 device is a thin reader client. All expensive Page reasoning, spoiler analysis, retrieval, shared-universe logic, and Gemini calls remain server-side.

Reuse CrossPoint's existing:

- EPUB reader;
- text selection;
- highlights/clippings;
- source-level reading position;
- dictionary;
- keyboard;
- FreeInkUI;
- Wi-Fi stack;
- SD storage;
- sleep/wake;
- physical page buttons;
- touchscreen behavior.

---

## 2. Keep ReadBookY Code Isolated

Do not scatter ReadBookY-specific logic throughout CrossPoint.

Create a dedicated module structure conceptually similar to:

```text
src/readbooky/
    ReadBookYClient.*
    ReadBookYAuth.*
    ReadBookYStorage.*
    ReadBookYSync.*
    ReadBookYOutbox.*
    ReadBookYAnchors.*
    ReadBookYPage.*
    ReadBookYModels.*

src/activities/readbooky/
    ReadBookYLoginActivity.*
    ReadBookYHomeActivity.*
    ReadBookYLibraryActivity.*
    ReadBookYDiscussionActivity.*
    ReadBookYPageActivity.*
    ReadBookYPageHistoryActivity.*
    ReadBookYSettingsActivity.*
```

Only add small integration hooks into CrossPoint where necessary:

```text
Home
Reader selection/context menu
Reader position events
Highlight/clipping events
Wi-Fi connection events
Sleep/wake
```

Use CrossPoint's existing UI framework for new screens.

Preserve mergeability with upstream CrossPoint.

---

## 3. Normal ReadBookY Login

The user must log in directly on the XTEINK using their ReadBookY credentials.

UI:

```text
READBOOKY

Email
[________________]

Password
[________________]

[ Sign in ]
```

Use CrossPoint's existing keyboard activity.

Authentication flow:

```text
email + password
     ↓
Firebase Authentication REST API
     ↓
Firebase ID token + refresh token + UID
     ↓
discard plaintext password
     ↓
persist authenticated session
```

Never store the plaintext password.

The user should remain signed in across:

- sleep;
- restart;
- reader navigation.

Require login again only after:

- explicit logout;
- revoked/invalid session;
- unrecoverable authentication failure.

---

## 4. Authentication Security

All ReadBookY authentication and private traffic must use verified HTTPS.

Do not use:

- insecure HTTP;
- `setInsecure()`;
- TLS without hostname verification;
- the existing CrossPoint HTTP relay for password/token traffic if it does not verify certificates.

ReadBookY API requests should carry:

```http
Authorization: Bearer <Firebase ID token>
```

Cloud Functions/backend endpoints verify the ID token with Firebase Admin SDK.

Session refresh must use the Firebase refresh token flow.

Store session credentials in internal device storage/NVS where practical, not in plaintext files on the user-visible SD card.

---

## 5. Device Registration

Authentication and device identity are separate.

After login, register the device:

```json
{
  "deviceId": "...",
  "deviceType": "xteink-x4-pro",
  "firmware": "...",
  "readbookyClientVersion": "...",
  "displayName": "XTEINK X4 Pro"
}
```

Store device information under the authenticated user's ReadBookY account.

This supports future device management such as:

```text
Devices

XTEINK X4 Pro
Last synced 3 minutes ago
```

Device identity must never replace user authentication.

---

## 6. ReadBookY Home / Library

Add **ReadBookY** to the CrossPoint home screen.

Opening it should show:

```text
ReadBookY

Continue Reading
────────────────
Stone Blind                37%
Queen of Shadows           11%

Library
────────────────
Crown of Midnight
Heir of Fire
...
```

The backend must return only books/journeys the authenticated user is authorized to access.

Selecting an undownloaded book:

```text
Stone Blind
Natalie Haynes

[ Download ]
```

Download EPUBs directly to SD without buffering the whole file in RAM.

Recommended path:

```text
/Books/ReadBookY/Stone Blind.epub
```

Store a ReadBookY sidecar:

```text
/Books/ReadBookY/Stone Blind.epub
/Books/ReadBookY/Stone Blind.epub.readbooky.json
```

Example:

```json
{
  "workId": "...",
  "editionId": "...",
  "journeyId": "...",
  "epubSha256": "...",
  "title": "Stone Blind",
  "author": "Natalie Haynes"
}
```

The EPUB SHA-256 should match the ReadBookY edition whenever possible.

---

## 7. Current Reading Position

Reuse CrossPoint's existing position structures rather than relying only on percentages.

Send enough data for ReadBookY to resolve the XTEINK location to the same canonical story location used by Page.

Conceptually:

```json
{
  "editionId": "...",
  "spineIndex": 12,
  "chapterHref": "Text/ch12.xhtml",
  "sourceOffset": 18422,
  "percentage": 0.37,
  "navigationReason": "sequential"
}
```

Possible `navigationReason` values:

```text
sequential
toc
search
bookmark
page_jump
footnote
external
```

---

## 8. Trusted Reading Position

Page must preserve the existing ReadBookY distinction:

```text
current view
≠
trusted reading position
```

Normal sequential reading may advance the trusted boundary.

These must not automatically advance it:

- TOC jumps;
- search;
- bookmarks;
- temporary look-ahead;
- index/glossary navigation;
- footnotes/endnotes;
- arbitrary distant jumps.

Moving backward never lowers the trusted boundary.

The backend should convert CrossPoint's source-level position into Page's canonical ordinal system.

---

## 9. Highlights

Reuse CrossPoint's native selection/highlight representation.

Do not anchor highlights to rendered page numbers.

For each ReadBookY highlight maintain:

```json
{
  "highlightId": "...",
  "editionId": "...",
  "chapterHref": "...",
  "startOffset": 0,
  "endOffset": 0,
  "selectedText": "...",
  "contextBefore": "...",
  "contextAfter": "...",
  "canonicalStartOrdinal": 0,
  "canonicalEndOrdinal": 0,
  "visibility": "public",
  "createdAt": "...",
  "updatedAt": "...",
  "deleted": false
}
```

Generate stable client-side IDs before sync.

Visibility:

```text
Public
Private
```

Private content must never be returned to another user.

---

## 10. Two-Way Highlight Translation

A highlight created on XTEINK should appear in ReadBookY web.

A compatible highlight created in ReadBookY web should appear on XTEINK.

Translate between:

```text
EPUB.js CFI
↕
canonical ordinal/text anchor
↕
CrossPoint chapter + source offset
```

Resolution priority:

```text
1. exact edition + canonical anchor
2. normalized text position
3. selected text + context before/after
4. unresolved instead of attaching to the wrong passage
```

Never silently attach an annotation to an ambiguous match.

---

## 11. Comments, Replies, Reactions

The selection/highlight experience should expose:

```text
Look Up
Highlight
Comment
Ask Page
```

Existing ReadBookY highlights should also expose:

```text
View Discussion
```

Discussion screen example:

```text
Highlighted passage

────────────────────────

Alex
I don't trust him here.

    Sam
    Same. Especially after chapter 4.

❤️ 2   😂 1

[ Add comment ]
```

Support:

- comments;
- replies;
- reactions;
- edits;
- deletes;
- public/private ownership rules.

Do not require connectivity to create or modify these.

---

## 12. Offline-First Local Storage

The XTEINK must remain usable as a ReadBookY reader without Wi-Fi.

Use a dedicated application directory on SD:

```text
/.readbooky/
    books/
    discussions/
    page/
    outbox/
    sync/
```

Keep authentication secrets/session material out of normal user-visible SD files when possible.

Use safe writes:

```text
write temp
close
rename atomically
```

Avoid rewriting giant JSON files repeatedly.

---

## 13. Durable Mutation Outbox

Every user mutation is local-first.

Supported operations include:

```text
create_highlight
update_highlight
delete_highlight

create_comment
edit_comment
delete_comment

create_reply
edit_reply
delete_reply

add_reaction
remove_reaction

update_progress
```

Each operation gets a stable `operationId`.

Example:

```json
{
  "operationId": "uuid",
  "type": "create_comment",
  "entityId": "comment_uuid",
  "highlightId": "highlight_uuid",
  "createdAtLocal": 1795050000,
  "payload": {},
  "status": "pending"
}
```

UI must update immediately.

Never wait for Firebase before showing the user's own change.

Persist the outbox before reporting success to the UI.

On reconnect:

```text
pending
→ upload
→ server idempotently processes operationId
→ acknowledge
→ remove/mark complete
```

Do not deduplicate comments by text.

---

## 14. Delta Synchronization

Do not download all social state every time.

Maintain server cursors.

Conceptually:

```text
POST /device/v1/sync
```

Request:

```json
{
  "deviceId": "...",
  "book": "...",
  "cursor": "...",
  "operations": []
}
```

Response:

```json
{
  "cursor": "...",
  "more": false,
  "changes": {
    "highlights": [],
    "comments": [],
    "replies": [],
    "reactions": []
  },
  "operationResults": []
}
```

Batch responses and cap payload sizes for ESP32 memory constraints.

Sync automatically when Wi-Fi is already available.

Also provide:

```text
ReadBookY → Sync now
```

Do not continuously wake Wi-Fi in the background.

Optional setting:

```text
Sync briefly before sleep
```

Default OFF.

---

## 15. Conflict Handling

Use stable IDs and deterministic revisions.

Server timestamps become authoritative after successful sync.

Deletes should use tombstones long enough for offline devices to converge.

If an operation is rejected because permissions changed:

- clearly mark it;
- preserve local content long enough for recovery/export;
- never silently discard user-authored text.

---

## 16. Offline Dictionary

Do not recreate the web dictionary.

Keep CrossPoint's native offline StarDict implementation.

Selection action:

```text
Look Up
```

must work with Wi-Fi off.

Optional ReadBookY settings UI may provide:

```text
Dictionary

[ Install English Dictionary ]
```

which downloads an approved/licensed StarDict pack into the normal CrossPoint dictionary directory.

After installation, lookups remain fully local.

---

## 17. Ask Page From Selected Text

Modify the reader selection menu to include:

```text
Look Up
Highlight
Comment
Ask Page
```

Do not rebuild text selection.

Reuse the selection object CrossPoint already created.

Pass:

```json
{
  "editionId": "...",
  "selectedText": "...",
  "chapterHref": "...",
  "startOffset": 0,
  "endOffset": 0,
  "canonicalOrdinal": 0,
  "trustedOrdinal": 0
}
```

---

## 18. Page UI

Opening Page from selected text:

```text
PAGE

"Hephaestus turned away..."

What would you like to know?

[ Who is this? ]
[ Did I miss something? ]
[ Explain this passage ]
[ Ask my own question ]
```

`Ask my own question` opens the existing CrossPoint keyboard.

Also provide a Page entry point from reader chrome/menu so Page can be used without selecting text.

Examples:

```text
Who is Cassandra again?
How far is Troy from Athens?
Why is Zeus having headaches?
```

---

## 19. Page Backend

Do not reproduce Page reasoning on the XTEINK.

Flow:

```text
XTEINK
   ↓
authenticated Page request
   ↓
resolve device anchor
   ↓
trusted ordinal
   ↓
safe retrieval
   ↓
series/shared-universe policy
   ↓
reveal ledger
   ↓
minimum necessary disclosure
   ↓
Gemini
   ↓
safe answer
```

All spoiler rules in `PAGE_SPEC.md` remain authoritative.

This includes:

- current-book trusted ordinal;
- prior-series logic;
- shared-universe/crossover policy;
- future-book blocking;
- meta-spoiler protection;
- minimum necessary disclosure;
- explicit spoiler override only when requested.

---

## 20. Offline Page Questions

Page reasoning requires the backend, but asking Page should not require connectivity.

If offline:

```text
Page is offline.

[ Queue question ]
[ Cancel ]
```

Store:

```json
{
  "requestId": "...",
  "question": "...",
  "selectedText": "...",
  "anchor": {},
  "trustedOrdinalAtQuestion": 18422,
  "askedAt": "..."
}
```

`trustedOrdinalAtQuestion` is mandatory.

When the queued request is eventually sent, Page must answer using the spoiler boundary from when the question was asked, not the reader's later location.

On reconnect:

```text
queued question
→ Page backend
→ response cached
```

Then show:

```text
Page found an answer
```

---

## 21. Page History

Cache completed Page Q&A locally.

Example:

```text
Page History

Who is Cassandra again?
Answered

Why doesn't Medusa have snakes yet?
Answered

How far is Troy from Athens?
Waiting for connection
```

Opening an already-completed answer must not require Wi-Fi.

---

## 22. Async Page Behavior

Do not block reading while Page works.

After submission:

```text
Page is thinking...
[ Back to book ]
```

The user may continue reading immediately.

When complete:

```text
Page found an answer
```

Use a subtle notification/badge.

Do not force the reader out of the book.

---

## 23. ReadBookY Status

Keep status minimal for e-ink.

Examples:

```text
ReadBookY ✓
```

or:

```text
ReadBookY
3 changes waiting
```

Sync screen:

```text
Last synced: 6:32 PM
Pending changes: 3
Page questions waiting: 1

[ Sync now ]
```

Avoid animated indicators.

---

## 24. E-Ink UI Rules

All ReadBookY UI must be designed for e-ink:

- no animations;
- no translucent overlays;
- no continuous spinners;
- strong black/white contrast;
- minimal refreshes;
- full refresh only when useful;
- physical buttons keep working;
- touchscreen navigation works;
- Back returns to the exact reading position;
- Page answers should use paged text rather than scroll-heavy layouts.

Do not make it look like a shrunken mobile website.

---

## 25. Memory Discipline

Respect ESP32-S3 constraints.

Use:

- streaming downloads;
- bounded JSON;
- paginated library APIs;
- delta synchronization;
- reusable buffers;
- SD-backed data;
- low-allocation hot paths.

Avoid:

- loading whole EPUBs into RAM;
- loading full discussion histories into RAM;
- giant transient strings;
- giant API responses;
- rewriting huge state files repeatedly.

---

## 26. Embedded REST API

The XTEINK should use authenticated HTTPS REST endpoints designed for embedded clients.

Conceptually:

```text
GET  /device/v1/library
GET  /device/v1/books/{editionId}/download

POST /device/v1/sync
POST /device/v1/progress

POST /device/v1/page/questions
GET  /device/v1/page/questions/{requestId}
GET  /device/v1/page/history
```

Every endpoint except Firebase Authentication itself verifies:

```text
Authorization: Bearer <Firebase ID token>
```

Backend authorization must verify:

```text
authenticated user
AND
user has access to requested journey/book
AND
edition belongs to expected work
```

Never trust device-supplied IDs without validation.

---

## 27. Protected Page Knowledge

The XTEINK must never download:

- reveal ledgers;
- future chapters;
- future-book aliases;
- protected Page compiler data;
- unrestricted story encyclopedia records.

The device receives only:

- safe generated answers;
- safe citations;
- safe display metadata.

---

## 28. Preserve Normal CrossPoint

A signed-out user must still have a normal CrossPoint reader.

Do not break:

- local EPUBs;
- native highlights/clippings;
- dictionary;
- OPDS;
- File Transfer;
- settings;
- sleep/wake;
- page buttons;
- touchscreen controls.

ReadBookY is an integration layer, not a replacement OS.

---

## 29. Acceptance Tests

### Login

- Open ReadBookY on X4 Pro.
- Enter email/password.
- Authenticate successfully.
- Restart device.
- Remain logged in.
- Plaintext password must not be recoverable from SD.

### Library

- See authorized ReadBookY books.
- Download a book.
- Disconnect Wi-Fi.
- Open and read it normally.

### Progress

- Read sequentially.
- Web ReadBookY sees progress.
- Search far ahead.
- Trusted spoiler boundary does not jump.
- Resume sequential reading.
- Trusted boundary advances normally.

### Dictionary

- Disable Wi-Fi.
- Select a word.
- Tap Look Up.
- Definition appears.

### Highlight

- Disable Wi-Fi.
- Create highlight.
- Reboot.
- Highlight remains.
- Reconnect.
- Highlight appears in ReadBookY web exactly once.

### Comments

- Offline, add comment.
- Add reply.
- Edit comment.
- Reboot.
- All remain locally.
- Reconnect.
- Sync completes.
- Web sees each exactly once.

### Reverse Sync

- Create a comment/highlight in web.
- Sync XTEINK.
- Cached discussion appears on device.

### Privacy

- Create private highlight/comment.
- Another user never receives it.

### Delete

- Delete offline.
- Reconnect.
- Deletion converges everywhere.

### Page Online

- Select passage.
- Ask Page.
- Continue reading while it works.
- Receive "Page found an answer".
- Answer respects trusted spoiler boundary.

### Page Offline

- Disconnect Wi-Fi.
- Ask Page.
- Queue question.
- Continue reading.
- Reconnect.
- Answer uses `trustedOrdinalAtQuestion`.
- Answer is available in Page History.

### Restart Recovery

- Create several unsynced operations.
- Hard reboot.
- Reconnect.
- Outbox resumes without duplicates or data loss.

---

## 30. Important Architectural Rule

Do not build ReadBookY authentication on CrossPoint Sync authentication.

Do not use MD5 username/password compatibility protocols for ReadBookY accounts.

Reuse useful CrossPoint position/clipping structures internally, but ReadBookY authentication and synchronization must use separate Firebase-authenticated APIs.

---

## 31. Hardware / Build Constraint

Target specifically:

```text
XTEINK X4 Pro
ESP32-S3
PlatformIO environment: x4pro
```

Do not build or flash the X3/X4 target.

Antigravity may compile firmware but must not automatically flash the physical device.

Do not run:

```text
pio run -e x4pro -t upload
```

without explicit user approval.

Manual test firmware installation remains user-controlled.

---

## 32. Source of Truth

This file defines the XTEINK integration.

For Page behavior, spoiler protection, trusted reading position, series handling, shared-universe logic, and minimum necessary disclosure, the canonical source of truth is:

```text
readbooky/docs/PAGE_SPEC.md
```

When the two documents overlap:

- this file defines device integration behavior;
- `PAGE_SPEC.md` defines Page intelligence/spoiler behavior.

---

## 33. Guiding Product Goal

The final result should feel like:

> ReadBookY has its own tiny e-reader.

Not:

> CrossPoint happens to have a Page button.

The X4 Pro should be a first-class ReadBookY device with local-first reading, comments, highlights, dictionary access, sync, and Page—while still remaining a fully functional CrossPoint reader.
