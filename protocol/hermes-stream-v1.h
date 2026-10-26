/*
 * hermes-stream-v1: Gamescope `--backend hermes` <-> Hermes streaming host.
 *
 * Gamescope composites every frame straight into a small pool of dma-bufs it
 * shares with one connected client, and takes keyboard, mouse, touch and text
 * input back on the same socket. Nothing goes through KMS, a seat, logind,
 * uinput or PipeWire, so the session can run beside the host desktop without
 * touching its displays or devices.
 *
 * Transport
 *   AF_UNIX, SOCK_SEQPACKET. One datagram is one message, beginning with
 *   struct hermes_msg_header. File descriptors travel as SCM_RIGHTS ancillary
 *   data on the message that names them. Both ends run on the same host, so
 *   every field is in host byte order and structs are laid out naturally (the
 *   static asserts at the bottom pin the sizes).
 *
 *   Gamescope listens on the path given by --hermes-socket (or
 *   $GAMESCOPE_HERMES_SOCKET, default $XDG_RUNTIME_DIR/gamescope-hermes-<pid>)
 *   and serves one client at a time. A newer connection replaces the old one
 *   as soon as its HELLO is accepted; one that closes or sends anything else
 *   first is dropped and leaves the current client streaming.
 *   The game keeps running while nobody is connected; Gamescope just stops
 *   compositing frames for the stream.
 *
 * Handshake
 *   server -> HERMES_SERVER_HELLO          (sent as soon as the client connects)
 *   client -> HERMES_CLIENT_HELLO
 *   client -> HERMES_CLIENT_CONFIGURE      (size, rate, formats, HDR, slots)
 *   server -> HERMES_SERVER_STATE          (what is actually in effect)
 *   server -> HERMES_SERVER_POOL           (new buffer pool, invalidates older ones)
 *   server -> HERMES_SERVER_BUFFER  x N    (one per slot, 1 fd each)
 *   server -> HERMES_SERVER_FRAME   ...    (slot + optional sync_file fd)
 *   client -> HERMES_CLIENT_RELEASE        (slot may be written again)
 *
 *   A CONFIGURE may be sent again at any time, e.g. when the viewer changes
 *   resolution. When the result needs different buffers the server sends a new
 *   POOL with a higher pool_id; frames always name the pool they belong to and
 *   the client drops buffers of older pools once it has no frame of theirs left.
 *
 * Buffer ownership
 *   A slot named by a FRAME belongs to the client until it sends RELEASE for
 *   it. Gamescope never writes a slot the client owns. If every slot is owned
 *   when a frame is ready, that frame is skipped and Gamescope repaints at the
 *   next vblank, so a slow consumer costs frames, never tearing. RELEASE only
 *   after the GPU work that reads the slot has finished (or after an explicit
 *   copy), as there is no release fence in v1.
 *
 * Synchronization
 *   When HERMES_FRAME_FLAG_FENCE is set the FRAME carries a sync_file fd that
 *   signals when composition into the slot is complete. Wait on it (poll() for
 *   POLLIN, or import it into EGL/Vulkan) before reading the buffer. Without
 *   the flag the buffer is already complete when the message is sent.
 *
 * Input
 *   Key and button codes are Linux evdev codes (KEY_*, BTN_*). Absolute
 *   coordinates are normalized to [0, 1] across the streamed image. Scroll uses
 *   wheel units of 120 per detent with Wayland's orientation (positive = down
 *   or right). Text is UTF-8 and is typed through Gamescope's own input method,
 *   so it works whatever keymap the game uses.
 */

#ifndef HERMES_STREAM_V1_H
#define HERMES_STREAM_V1_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HERMES_STREAM_VERSION_MAJOR 1
#define HERMES_STREAM_VERSION_MINOR 0

/* Largest message either side sends, ancillary data excluded. */
#define HERMES_MAX_MESSAGE_SIZE 4096

#define HERMES_MAX_FORMATS 128
#define HERMES_MAX_PLANES 4
#define HERMES_MIN_SLOTS 2
#define HERMES_MAX_SLOTS 8
#define HERMES_MAX_TEXT 256

enum hermes_msg_type
{
	/* server -> client */
	HERMES_SERVER_HELLO = 1,
	HERMES_SERVER_ERROR = 2,
	HERMES_SERVER_STATE = 3,
	HERMES_SERVER_POOL = 4,
	HERMES_SERVER_BUFFER = 5,
	HERMES_SERVER_FRAME = 6,

	/* client -> server */
	HERMES_CLIENT_HELLO = 64,
	HERMES_CLIENT_CONFIGURE = 65,
	HERMES_CLIENT_RELEASE = 66,
	HERMES_CLIENT_REQUEST_FRAME = 67,

	HERMES_CLIENT_POINTER_MOTION = 80,
	HERMES_CLIENT_POINTER_ABSOLUTE = 81,
	HERMES_CLIENT_POINTER_BUTTON = 82,
	HERMES_CLIENT_POINTER_AXIS = 83,
	HERMES_CLIENT_KEY = 84,
	HERMES_CLIENT_TOUCH = 85,
	HERMES_CLIENT_TEXT = 86,
};

struct hermes_msg_header
{
	uint16_t type;     /* enum hermes_msg_type */
	uint16_t reserved; /* 0 */
	uint32_t size;     /* whole message, header included */
};

/* ---------------------------------------------------------------------------
 * server -> client
 * ------------------------------------------------------------------------- */

enum hermes_server_caps
{
	/* FRAMEs can carry a sync_file. Without it every FRAME is complete on arrival. */
	HERMES_SERVER_CAP_SYNC_FILE = 1u << 0,
	/* DRM format modifiers other than LINEAR can be exported. */
	HERMES_SERVER_CAP_MODIFIERS = 1u << 1,
	/* DRM_FORMAT_NV12 can be produced directly (linear). */
	HERMES_SERVER_CAP_NV12 = 1u << 2,
	/* HDR10 (PQ, BT.2020) output is possible. */
	HERMES_SERVER_CAP_HDR10 = 1u << 3,
	/* HERMES_CLIENT_TEXT is supported. */
	HERMES_SERVER_CAP_TEXT = 1u << 4,
};

struct hermes_server_hello
{
	struct hermes_msg_header hdr;
	uint16_t version_major;
	uint16_t version_minor;
	uint32_t caps;             /* enum hermes_server_caps */
	uint32_t width;            /* current output size */
	uint32_t height;
	uint32_t refresh_mhz;
	uint32_t render_dev_major; /* the render node Gamescope composites on, */
	uint32_t render_dev_minor; /* 0/0 when unknown */
	uint32_t reserved;
	char gamescope_version[64];
};

enum hermes_error_code
{
	HERMES_ERROR_VERSION = 1,       /* incompatible major version, connection closes */
	HERMES_ERROR_PROTOCOL = 2,      /* malformed message, connection closes */
	HERMES_ERROR_NO_FORMAT = 3,     /* nothing in CONFIGURE could be produced */
	HERMES_ERROR_ALLOCATION = 4,    /* buffers could not be created */
	HERMES_ERROR_INVALID_SLOT = 5,  /* RELEASE named a slot the client did not own */
};

struct hermes_server_error
{
	struct hermes_msg_header hdr;
	uint32_t code;  /* enum hermes_error_code */
	uint32_t fatal; /* 1 when the server closes the connection after this */
	char message[128];
};

enum hermes_state_flags
{
	HERMES_STATE_HDR_ACTIVE = 1u << 0, /* frames are currently PQ / BT.2020 */
	HERMES_STATE_HDR_CAPABLE = 1u << 1, /* games are told HDR is available */
};

struct hermes_server_state
{
	struct hermes_msg_header hdr;
	uint32_t width;
	uint32_t height;
	uint32_t refresh_mhz;
	uint32_t flags;        /* enum hermes_state_flags */
	uint32_t nested_width; /* size the game renders at before scaling */
	uint32_t nested_height;
};

enum hermes_pool_flags
{
	/* Each slot holds a single allocation; every plane uses the slot's one fd. */
	HERMES_POOL_SINGLE_ALLOCATION = 1u << 0,
};

struct hermes_server_pool
{
	struct hermes_msg_header hdr;
	uint32_t pool_id;     /* increases with every pool; 0 is never used */
	uint32_t slot_count;
	uint32_t width;
	uint32_t height;
	uint32_t drm_format;  /* DRM_FORMAT_* fourcc */
	uint32_t flags;       /* enum hermes_pool_flags */
	uint64_t modifier;    /* DRM_FORMAT_MOD_*, LINEAR for NV12 */
};

/* Carries exactly one dma-buf fd. */
struct hermes_server_buffer
{
	struct hermes_msg_header hdr;
	uint32_t pool_id;
	uint32_t slot;
	uint32_t num_planes;
	uint32_t reserved;
	uint32_t offsets[HERMES_MAX_PLANES];
	uint32_t strides[HERMES_MAX_PLANES];
	uint64_t allocation_size; /* size of the dma-buf, 0 when unknown */
};

enum hermes_frame_flags
{
	HERMES_FRAME_FLAG_FENCE = 1u << 0,    /* a sync_file fd is attached */
	HERMES_FRAME_FLAG_HDR = 1u << 1,      /* hdr_* fields are valid */
	HERMES_FRAME_FLAG_REQUESTED = 1u << 2, /* produced for REQUEST_FRAME or a new CONFIGURE; may repeat the last content */
};

enum hermes_transfer
{
	HERMES_TRANSFER_GAMMA22 = 0, /* SDR, encoded for a 2.2 gamma display (sRGB-like) */
	HERMES_TRANSFER_PQ = 1,      /* SMPTE ST 2084 */
};

enum hermes_primaries
{
	HERMES_PRIMARIES_BT709 = 0,
	HERMES_PRIMARIES_BT2020 = 1,
};

enum hermes_yuv_colorspace
{
	HERMES_YUV_BT601_LIMITED = 1,
	HERMES_YUV_BT601_FULL = 2,
	HERMES_YUV_BT709_LIMITED = 3,
	HERMES_YUV_BT709_FULL = 4,
};

struct hermes_server_frame
{
	struct hermes_msg_header hdr;
	uint32_t pool_id;
	uint32_t slot;
	uint64_t frame_id;       /* increases by one per FRAME sent on this connection */
	uint64_t submit_time_ns; /* CLOCK_MONOTONIC when composition was queued on the GPU */
	uint32_t flags;          /* enum hermes_frame_flags */
	uint32_t transfer;       /* enum hermes_transfer */
	uint32_t primaries;      /* enum hermes_primaries */
	uint32_t yuv_colorspace; /* enum hermes_yuv_colorspace, NV12 only */

	/* HDR static metadata (CTA-861.3), valid with HERMES_FRAME_FLAG_HDR. */
	uint16_t hdr_display_primaries_x[3]; /* units of 0.00002, R G B */
	uint16_t hdr_display_primaries_y[3];
	uint16_t hdr_white_point_x;
	uint16_t hdr_white_point_y;
	uint16_t hdr_max_display_mastering_luminance; /* cd/m^2 */
	uint16_t hdr_min_display_mastering_luminance; /* 0.0001 cd/m^2 */
	uint16_t hdr_max_cll;  /* cd/m^2 */
	uint16_t hdr_max_fall; /* cd/m^2 */
};

/* ---------------------------------------------------------------------------
 * client -> server
 * ------------------------------------------------------------------------- */

struct hermes_client_hello
{
	struct hermes_msg_header hdr;
	uint16_t version_major;
	uint16_t version_minor;
	uint32_t caps; /* reserved, 0 */
	char client_name[64];
};

struct hermes_format_modifier
{
	uint32_t drm_format;
	uint32_t reserved;
	uint64_t modifier; /* DRM_FORMAT_MOD_* the client can import for this format */
};

enum hermes_configure_flags
{
	/* Tell games the display supports HDR10. Frames switch to PQ / BT.2020
	 * while HDR is also enabled in Gamescope (--hdr-enabled, or Steam's
	 * setting in -e mode), which then needs an RGB 10-bit format in the list.
	 * Games see the change on their next swapchain or HDR query, so set this
	 * before launching anything that should run in HDR. */
	HERMES_CONFIGURE_HDR = 1u << 0,
	/* Resize the game's X screen along with the output. Without it a size
	 * change only rescales the game's image. */
	HERMES_CONFIGURE_RESIZE_NESTED = 1u << 1,
};

/* Fixed header followed by num_formats hermes_format_modifier entries, best first. */
struct hermes_client_configure
{
	struct hermes_msg_header hdr;
	uint32_t width;          /* 0 keeps the current size */
	uint32_t height;
	uint32_t refresh_mhz;    /* 0 keeps the current rate */
	uint32_t flags;          /* enum hermes_configure_flags */
	uint32_t slot_count;     /* HERMES_MIN_SLOTS..HERMES_MAX_SLOTS, 0 = 3 */
	uint32_t yuv_colorspace; /* enum hermes_yuv_colorspace for NV12, 0 = BT709_LIMITED */
	uint32_t num_formats;
	/* The viewer's display, for HDR tone mapping. 0 picks a default
	 * (1000 cd/m^2 peak, 400 frame average, 0 black). */
	uint16_t hdr_max_luminance;      /* cd/m^2 */
	uint16_t hdr_max_frame_average;  /* cd/m^2 */
	uint16_t hdr_min_luminance;      /* 0.0001 cd/m^2 */
	uint16_t reserved[3];
	/* struct hermes_format_modifier formats[num_formats]; */
};

struct hermes_client_release
{
	struct hermes_msg_header hdr;
	uint32_t pool_id;
	uint32_t slot;
};

/* Composite and send a frame at the next vblank even if nothing changed,
 * e.g. to feed an encoder that needs a fresh keyframe. */
struct hermes_client_request_frame
{
	struct hermes_msg_header hdr;
};

struct hermes_client_pointer_motion
{
	struct hermes_msg_header hdr;
	double dx;
	double dy;
};

struct hermes_client_pointer_absolute
{
	struct hermes_msg_header hdr;
	double x; /* 0..1 across the streamed image */
	double y;
};

struct hermes_client_pointer_button
{
	struct hermes_msg_header hdr;
	uint32_t button;  /* BTN_LEFT, BTN_RIGHT, ... */
	uint32_t pressed; /* 1 = down, 0 = up */
};

struct hermes_client_pointer_axis
{
	struct hermes_msg_header hdr;
	int32_t value120_x; /* 120 per detent, positive = right */
	int32_t value120_y; /* 120 per detent, positive = down */
};

struct hermes_client_key
{
	struct hermes_msg_header hdr;
	uint32_t keycode; /* KEY_* */
	uint32_t pressed;
};

enum hermes_touch_type
{
	HERMES_TOUCH_DOWN = 0,
	HERMES_TOUCH_MOTION = 1,
	HERMES_TOUCH_UP = 2,
	HERMES_TOUCH_CANCEL = 3, /* lifts every contact */
};

struct hermes_client_touch
{
	struct hermes_msg_header hdr;
	uint32_t type; /* enum hermes_touch_type */
	int32_t id;
	double x;      /* 0..1, ignored for UP and CANCEL */
	double y;
};

/* Fixed header followed by `length` bytes of UTF-8, not NUL terminated. */
struct hermes_client_text
{
	struct hermes_msg_header hdr;
	uint32_t length; /* at most HERMES_MAX_TEXT */
	uint32_t reserved;
	/* char text[length]; */
};

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
#define HERMES_STATIC_ASSERT( expr, msg ) static_assert( expr, msg )
#else
#define HERMES_STATIC_ASSERT( expr, msg ) _Static_assert( expr, msg )
#endif

HERMES_STATIC_ASSERT( sizeof( struct hermes_msg_header ) == 8, "hermes_msg_header layout" );
HERMES_STATIC_ASSERT( sizeof( struct hermes_server_hello ) == 104, "hermes_server_hello layout" );
HERMES_STATIC_ASSERT( sizeof( struct hermes_server_error ) == 144, "hermes_server_error layout" );
HERMES_STATIC_ASSERT( sizeof( struct hermes_server_state ) == 32, "hermes_server_state layout" );
HERMES_STATIC_ASSERT( sizeof( struct hermes_server_pool ) == 40, "hermes_server_pool layout" );
HERMES_STATIC_ASSERT( sizeof( struct hermes_server_buffer ) == 64, "hermes_server_buffer layout" );
HERMES_STATIC_ASSERT( sizeof( struct hermes_server_frame ) == 72, "hermes_server_frame layout" );
HERMES_STATIC_ASSERT( sizeof( struct hermes_client_hello ) == 80, "hermes_client_hello layout" );
HERMES_STATIC_ASSERT( sizeof( struct hermes_format_modifier ) == 16, "hermes_format_modifier layout" );
HERMES_STATIC_ASSERT( sizeof( struct hermes_client_configure ) == 48, "hermes_client_configure layout" );
HERMES_STATIC_ASSERT( sizeof( struct hermes_client_release ) == 16, "hermes_client_release layout" );
HERMES_STATIC_ASSERT( sizeof( struct hermes_client_pointer_motion ) == 24, "hermes_client_pointer_motion layout" );
HERMES_STATIC_ASSERT( sizeof( struct hermes_client_pointer_absolute ) == 24, "hermes_client_pointer_absolute layout" );
HERMES_STATIC_ASSERT( sizeof( struct hermes_client_pointer_button ) == 16, "hermes_client_pointer_button layout" );
HERMES_STATIC_ASSERT( sizeof( struct hermes_client_pointer_axis ) == 16, "hermes_client_pointer_axis layout" );
HERMES_STATIC_ASSERT( sizeof( struct hermes_client_key ) == 16, "hermes_client_key layout" );
HERMES_STATIC_ASSERT( sizeof( struct hermes_client_touch ) == 32, "hermes_client_touch layout" );
HERMES_STATIC_ASSERT( sizeof( struct hermes_client_text ) == 16, "hermes_client_text layout" );
HERMES_STATIC_ASSERT( sizeof( struct hermes_client_configure ) + HERMES_MAX_FORMATS * sizeof( struct hermes_format_modifier ) <= HERMES_MAX_MESSAGE_SIZE, "configure fits a message" );

#undef HERMES_STATIC_ASSERT

#endif /* HERMES_STREAM_V1_H */
