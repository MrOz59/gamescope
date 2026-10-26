// gamescope-hermes-client: reference client for `gamescope --backend hermes`.
//
// Connects to the stream socket, configures the stream, waits on every frame's
// fence, releases the buffer and prints what it saw. It can dump a frame to a
// file and type text into the session, which is enough to check a session end
// to end without Hermes. It only needs libc, and only linear buffers can be
// dumped (the defaults ask for linear ones).

#define _GNU_SOURCE

#include "../../protocol/hermes-stream-v1.h"

#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include <linux/dma-buf.h>

#define FOURCC( a, b, c, d ) ( (uint32_t)(a) | ( (uint32_t)(b) << 8 ) | ( (uint32_t)(c) << 16 ) | ( (uint32_t)(d) << 24 ) )

#define FORMAT_XRGB8888 FOURCC( 'X', 'R', '2', '4' )
#define FORMAT_ARGB8888 FOURCC( 'A', 'R', '2', '4' )
#define FORMAT_XBGR8888 FOURCC( 'X', 'B', '2', '4' )
#define FORMAT_ABGR8888 FOURCC( 'A', 'B', '2', '4' )
#define FORMAT_XRGB2101010 FOURCC( 'X', 'R', '3', '0' )
#define FORMAT_XBGR2101010 FOURCC( 'X', 'B', '3', '0' )
#define FORMAT_ABGR2101010 FOURCC( 'A', 'B', '3', '0' )
#define FORMAT_NV12 FOURCC( 'N', 'V', '1', '2' )

#define MODIFIER_LINEAR 0ull
#define MODIFIER_INVALID 0x00ffffffffffffffull

struct format_name
{
	const char *name;
	uint32_t format;
};

static const struct format_name s_formats[] =
{
	{ "nv12", FORMAT_NV12 },
	{ "xrgb8888", FORMAT_XRGB8888 },
	{ "argb8888", FORMAT_ARGB8888 },
	{ "xbgr8888", FORMAT_XBGR8888 },
	{ "abgr8888", FORMAT_ABGR8888 },
	{ "xrgb2101010", FORMAT_XRGB2101010 },
	{ "xbgr2101010", FORMAT_XBGR2101010 },
	{ "abgr2101010", FORMAT_ABGR2101010 },
};

struct buffer
{
	int fd;
	uint32_t num_planes;
	uint32_t offsets[HERMES_MAX_PLANES];
	uint32_t strides[HERMES_MAX_PLANES];
	uint64_t size;
};

struct client
{
	int fd;

	uint32_t pool_id;
	uint32_t width;
	uint32_t height;
	uint32_t format;
	uint64_t modifier;
	uint32_t slot_count;
	struct buffer buffers[HERMES_MAX_SLOTS];

	uint64_t frames;
	uint64_t frames_in_second;
	uint64_t last_frame_id;
	uint64_t dropped_ids;
	double fence_wait_ms;
	double latency_ms;
	uint64_t second_start_ns;
};

static uint64_t now_ns( void )
{
	struct timespec ts;
	clock_gettime( CLOCK_MONOTONIC, &ts );
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static const char *format_to_string( uint32_t format )
{
	for ( size_t i = 0; i < sizeof( s_formats ) / sizeof( s_formats[0] ); i++ )
	{
		if ( s_formats[i].format == format )
			return s_formats[i].name;
	}
	return "unknown";
}

static void close_buffers( struct client *client )
{
	for ( uint32_t i = 0; i < HERMES_MAX_SLOTS; i++ )
	{
		if ( client->buffers[i].fd >= 0 )
			close( client->buffers[i].fd );
		client->buffers[i].fd = -1;
	}
}

static bool send_message( struct client *client, const void *data, size_t size )
{
	ssize_t sent = send( client->fd, data, size, MSG_NOSIGNAL );
	if ( sent != (ssize_t)size )
	{
		fprintf( stderr, "send failed: %s\n", sent < 0 ? strerror( errno ) : "short write" );
		return false;
	}
	return true;
}

// Receives one message, and its fd if it carries one.
static ssize_t receive_message( struct client *client, void *data, size_t size, int *out_fd )
{
	char control[ CMSG_SPACE( sizeof( int ) * 4 ) ];
	struct iovec iov = { .iov_base = data, .iov_len = size };
	struct msghdr msg = {
		.msg_iov = &iov,
		.msg_iovlen = 1,
		.msg_control = control,
		.msg_controllen = sizeof( control ),
	};

	*out_fd = -1;

	ssize_t received = recvmsg( client->fd, &msg, MSG_CMSG_CLOEXEC );
	if ( received <= 0 )
		return received;

	for ( struct cmsghdr *cmsg = CMSG_FIRSTHDR( &msg ); cmsg; cmsg = CMSG_NXTHDR( &msg, cmsg ) )
	{
		if ( cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS )
			continue;

		size_t count = ( cmsg->cmsg_len - CMSG_LEN( 0 ) ) / sizeof( int );
		for ( size_t i = 0; i < count; i++ )
		{
			int fd;
			memcpy( &fd, CMSG_DATA( cmsg ) + i * sizeof( int ), sizeof( int ) );
			if ( *out_fd < 0 )
				*out_fd = fd;
			else
				close( fd );
		}
	}

	return received;
}

static bool send_text( struct client *client, const char *text )
{
	size_t length = strlen( text );
	if ( length > HERMES_MAX_TEXT )
		length = HERMES_MAX_TEXT;

	uint8_t data[ sizeof( struct hermes_client_text ) + HERMES_MAX_TEXT ];
	struct hermes_client_text msg = {
		.hdr = { .type = HERMES_CLIENT_TEXT, .size = (uint32_t)( sizeof( msg ) + length ) },
		.length = (uint32_t)length,
	};
	memcpy( data, &msg, sizeof( msg ) );
	memcpy( data + sizeof( msg ), text, length );
	return send_message( client, data, sizeof( msg ) + length );
}

// Writes the frame in slot `slot` to `path`: a PPM for RGB, raw planes for NV12.
static bool dump_frame( struct client *client, uint32_t slot, const char *path )
{
	struct buffer *buffer = &client->buffers[slot];
	if ( client->modifier != MODIFIER_LINEAR )
	{
		fprintf( stderr, "Can only dump linear buffers (modifier is 0x%" PRIx64 ")\n", client->modifier );
		return false;
	}

	size_t map_size = buffer->size;
	if ( !map_size )
	{
		off_t end = lseek( buffer->fd, 0, SEEK_END );
		map_size = end > 0 ? (size_t)end : 0;
	}
	if ( !map_size )
	{
		fprintf( stderr, "Unknown buffer size, cannot map it\n" );
		return false;
	}

	uint8_t *map = mmap( NULL, map_size, PROT_READ, MAP_SHARED, buffer->fd, 0 );
	if ( map == MAP_FAILED )
	{
		fprintf( stderr, "mmap failed: %s\n", strerror( errno ) );
		return false;
	}

	struct dma_buf_sync sync = { .flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ };
	ioctl( buffer->fd, DMA_BUF_IOCTL_SYNC, &sync );

	FILE *file = fopen( path, "wb" );
	bool ok = file != NULL;

	if ( ok && client->format == FORMAT_NV12 )
	{
		for ( uint32_t y = 0; y < client->height; y++ )
			fwrite( map + buffer->offsets[0] + (size_t)y * buffer->strides[0], 1, client->width, file );
		for ( uint32_t y = 0; y < ( client->height + 1 ) / 2; y++ )
			fwrite( map + buffer->offsets[1] + (size_t)y * buffer->strides[1], 1, ( client->width + 1 ) / 2 * 2, file );
	}
	else if ( ok )
	{
		fprintf( file, "P6\n%u %u\n255\n", client->width, client->height );
		for ( uint32_t y = 0; y < client->height; y++ )
		{
			const uint32_t *row = (const uint32_t *)( map + buffer->offsets[0] + (size_t)y * buffer->strides[0] );
			for ( uint32_t x = 0; x < client->width; x++ )
			{
				uint32_t pixel = row[x];
				uint8_t rgb[3];
				switch ( client->format )
				{
					case FORMAT_XRGB8888:
					case FORMAT_ARGB8888:
						rgb[0] = ( pixel >> 16 ) & 0xff; rgb[1] = ( pixel >> 8 ) & 0xff; rgb[2] = pixel & 0xff;
						break;
					case FORMAT_XBGR8888:
					case FORMAT_ABGR8888:
						rgb[0] = pixel & 0xff; rgb[1] = ( pixel >> 8 ) & 0xff; rgb[2] = ( pixel >> 16 ) & 0xff;
						break;
					case FORMAT_XRGB2101010:
						rgb[0] = ( pixel >> 22 ) & 0xff; rgb[1] = ( pixel >> 12 ) & 0xff; rgb[2] = ( pixel >> 2 ) & 0xff;
						break;
					default: // [X|A]BGR2101010
						rgb[0] = ( pixel >> 2 ) & 0xff; rgb[1] = ( pixel >> 12 ) & 0xff; rgb[2] = ( pixel >> 22 ) & 0xff;
						break;
				}
				fwrite( rgb, 1, 3, file );
			}
		}
	}

	if ( file )
		fclose( file );

	sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ;
	ioctl( buffer->fd, DMA_BUF_IOCTL_SYNC, &sync );
	munmap( map, map_size );

	if ( ok )
		printf( "Wrote %s (%ux%u %s)\n", path, client->width, client->height, format_to_string( client->format ) );
	return ok;
}

static void usage( const char *argv0 )
{
	fprintf( stderr,
		"usage: %s [options]\n"
		"  -s, --socket PATH     stream socket (default: $GAMESCOPE_HERMES_SOCKET)\n"
		"  -W, --width N         output width (default: keep)\n"
		"  -H, --height N        output height (default: keep)\n"
		"  -r, --refresh HZ      output refresh rate (default: keep)\n"
		"  -f, --format NAME     preferred format, repeatable, best first:\n"
		"                        nv12 xrgb8888 argb8888 xbgr8888 abgr8888 xrgb2101010 xbgr2101010 abgr2101010\n"
		"                        (default: nv12 xbgr8888 xrgb8888 abgr2101010)\n"
		"  -m, --modifier HEX    modifier accepted for every format, repeatable (default: linear)\n"
		"      --hdr             ask for HDR10\n"
		"      --resize-nested   resize the game's screen with the output\n"
		"      --slots N         buffers in the pool (2-8)\n"
		"  -n, --frames N        exit after N frames (default: run until interrupted)\n"
		"  -d, --dump PATH       write frame --dump-frame to PATH (.ppm for RGB, raw planes for NV12)\n"
		"      --dump-frame N    which frame to dump (default: 1)\n"
		"  -t, --text STRING     type STRING into the session once the stream starts\n",
		argv0 );
}

int main( int argc, char **argv )
{
	const char *socket_path = getenv( "GAMESCOPE_HERMES_SOCKET" );
	uint32_t width = 0, height = 0, refresh_mhz = 0, flags = 0, slots = 0;
	uint32_t formats[16];
	size_t format_count = 0;
	uint64_t modifiers[16];
	size_t modifier_count = 0;
	uint64_t max_frames = 0;
	const char *dump_path = NULL;
	uint64_t dump_frame_number = 1;
	const char *text = NULL;

	enum { OPT_HDR = 256, OPT_RESIZE_NESTED, OPT_SLOTS, OPT_DUMP_FRAME };
	static const struct option options[] =
	{
		{ "socket", required_argument, NULL, 's' },
		{ "width", required_argument, NULL, 'W' },
		{ "height", required_argument, NULL, 'H' },
		{ "refresh", required_argument, NULL, 'r' },
		{ "format", required_argument, NULL, 'f' },
		{ "modifier", required_argument, NULL, 'm' },
		{ "hdr", no_argument, NULL, OPT_HDR },
		{ "resize-nested", no_argument, NULL, OPT_RESIZE_NESTED },
		{ "slots", required_argument, NULL, OPT_SLOTS },
		{ "frames", required_argument, NULL, 'n' },
		{ "dump", required_argument, NULL, 'd' },
		{ "dump-frame", required_argument, NULL, OPT_DUMP_FRAME },
		{ "text", required_argument, NULL, 't' },
		{ "help", no_argument, NULL, 'h' },
		{ 0 },
	};

	int opt;
	while ( ( opt = getopt_long( argc, argv, "s:W:H:r:f:m:n:d:t:h", options, NULL ) ) != -1 )
	{
		switch ( opt )
		{
			case 's': socket_path = optarg; break;
			case 'W': width = (uint32_t)strtoul( optarg, NULL, 10 ); break;
			case 'H': height = (uint32_t)strtoul( optarg, NULL, 10 ); break;
			case 'r': refresh_mhz = (uint32_t)( strtod( optarg, NULL ) * 1000.0 + 0.5 ); break;
			case 'f':
			{
				bool found = false;
				for ( size_t i = 0; i < sizeof( s_formats ) / sizeof( s_formats[0] ); i++ )
				{
					if ( !strcmp( optarg, s_formats[i].name ) && format_count < 16 )
					{
						formats[format_count++] = s_formats[i].format;
						found = true;
					}
				}
				if ( !found )
				{
					fprintf( stderr, "Unknown format: %s\n", optarg );
					return 1;
				}
				break;
			}
			case 'm':
				if ( modifier_count < 16 )
					modifiers[modifier_count++] = strtoull( optarg, NULL, 16 );
				break;
			case OPT_HDR: flags |= HERMES_CONFIGURE_HDR; break;
			case OPT_RESIZE_NESTED: flags |= HERMES_CONFIGURE_RESIZE_NESTED; break;
			case OPT_SLOTS: slots = (uint32_t)strtoul( optarg, NULL, 10 ); break;
			case 'n': max_frames = strtoull( optarg, NULL, 10 ); break;
			case 'd': dump_path = optarg; break;
			case OPT_DUMP_FRAME: dump_frame_number = strtoull( optarg, NULL, 10 ); break;
			case 't': text = optarg; break;
			case 'h':
			default:
				usage( argv[0] );
				return opt == 'h' ? 0 : 1;
		}
	}

	if ( !socket_path || !*socket_path )
	{
		fprintf( stderr, "No socket: pass --socket or set GAMESCOPE_HERMES_SOCKET\n" );
		return 1;
	}

	if ( !format_count )
	{
		formats[format_count++] = FORMAT_NV12;
		formats[format_count++] = FORMAT_XBGR8888;
		formats[format_count++] = FORMAT_XRGB8888;
		formats[format_count++] = FORMAT_ABGR2101010;
	}
	if ( !modifier_count )
		modifiers[modifier_count++] = MODIFIER_LINEAR;

	struct client client = { .fd = -1 };
	for ( uint32_t i = 0; i < HERMES_MAX_SLOTS; i++ )
		client.buffers[i].fd = -1;

	client.fd = socket( AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0 );
	if ( client.fd < 0 )
	{
		fprintf( stderr, "socket failed: %s\n", strerror( errno ) );
		return 1;
	}

	struct sockaddr_un addr = { .sun_family = AF_UNIX };
	if ( strlen( socket_path ) >= sizeof( addr.sun_path ) )
	{
		fprintf( stderr, "Socket path too long\n" );
		return 1;
	}
	strcpy( addr.sun_path, socket_path );
	if ( connect( client.fd, (struct sockaddr *)&addr, sizeof( addr ) ) != 0 )
	{
		fprintf( stderr, "connect to %s failed: %s\n", socket_path, strerror( errno ) );
		return 1;
	}

	struct hermes_client_hello hello = {
		.hdr = { .type = HERMES_CLIENT_HELLO, .size = sizeof( hello ) },
		.version_major = HERMES_STREAM_VERSION_MAJOR,
		.version_minor = HERMES_STREAM_VERSION_MINOR,
	};
	snprintf( hello.client_name, sizeof( hello.client_name ), "gamescope-hermes-client" );
	if ( !send_message( &client, &hello, sizeof( hello ) ) )
		return 1;

	uint8_t configure_data[ sizeof( struct hermes_client_configure ) + 16 * 16 * sizeof( struct hermes_format_modifier ) ];
	struct hermes_client_configure configure = {
		.hdr = { .type = HERMES_CLIENT_CONFIGURE },
		.width = width,
		.height = height,
		.refresh_mhz = refresh_mhz,
		.flags = flags,
		.slot_count = slots,
		.yuv_colorspace = HERMES_YUV_BT709_LIMITED,
	};
	size_t entry_offset = sizeof( configure );
	for ( size_t f = 0; f < format_count; f++ )
	{
		for ( size_t m = 0; m < modifier_count; m++ )
		{
			struct hermes_format_modifier entry = { .drm_format = formats[f], .modifier = modifiers[m] };
			memcpy( configure_data + entry_offset, &entry, sizeof( entry ) );
			entry_offset += sizeof( entry );
			configure.num_formats++;
		}
	}
	configure.hdr.size = (uint32_t)entry_offset;
	memcpy( configure_data, &configure, sizeof( configure ) );
	if ( !send_message( &client, configure_data, entry_offset ) )
		return 1;

	bool text_sent = false;
	int status = 0;
	client.second_start_ns = now_ns();

	for ( ;; )
	{
		uint8_t data[ HERMES_MAX_MESSAGE_SIZE ];
		int fd = -1;
		ssize_t received = receive_message( &client, data, sizeof( data ), &fd );
		if ( received <= 0 )
		{
			fprintf( stderr, "Connection closed%s%s\n", received < 0 ? ": " : "", received < 0 ? strerror( errno ) : "" );
			status = 1;
			break;
		}

		struct hermes_msg_header header;
		memcpy( &header, data, sizeof( header ) );

		switch ( header.type )
		{
			case HERMES_SERVER_HELLO:
			{
				struct hermes_server_hello msg;
				memcpy( &msg, data, sizeof( msg ) );
				printf( "Server: gamescope %.*s, hermes-stream %u.%u, %ux%u@%.2fHz, caps 0x%x, render node %u:%u\n",
					(int)sizeof( msg.gamescope_version ), msg.gamescope_version, msg.version_major, msg.version_minor,
					msg.width, msg.height, msg.refresh_mhz / 1000.0, msg.caps, msg.render_dev_major, msg.render_dev_minor );
				break;
			}

			case HERMES_SERVER_ERROR:
			{
				struct hermes_server_error msg;
				memcpy( &msg, data, sizeof( msg ) );
				fprintf( stderr, "Server error %u%s: %.*s\n", msg.code, msg.fatal ? " (fatal)" : "", (int)sizeof( msg.message ), msg.message );
				break;
			}

			case HERMES_SERVER_STATE:
			{
				struct hermes_server_state msg;
				memcpy( &msg, data, sizeof( msg ) );
				printf( "State: %ux%u@%.2fHz, game %ux%u, HDR %s%s\n", msg.width, msg.height, msg.refresh_mhz / 1000.0,
					msg.nested_width, msg.nested_height,
					( msg.flags & HERMES_STATE_HDR_ACTIVE ) ? "active" : "inactive",
					( msg.flags & HERMES_STATE_HDR_CAPABLE ) ? " (capable)" : "" );
				break;
			}

			case HERMES_SERVER_POOL:
			{
				struct hermes_server_pool msg;
				memcpy( &msg, data, sizeof( msg ) );
				close_buffers( &client );
				client.pool_id = msg.pool_id;
				client.width = msg.width;
				client.height = msg.height;
				client.format = msg.drm_format;
				client.modifier = msg.modifier;
				client.slot_count = msg.slot_count;
				printf( "Pool %u: %ux%u %s, modifier 0x%" PRIx64 ", %u slots\n", msg.pool_id, msg.width, msg.height,
					format_to_string( msg.drm_format ), msg.modifier, msg.slot_count );
				break;
			}

			case HERMES_SERVER_BUFFER:
			{
				struct hermes_server_buffer msg;
				memcpy( &msg, data, sizeof( msg ) );
				if ( msg.pool_id != client.pool_id || msg.slot >= HERMES_MAX_SLOTS || fd < 0 )
				{
					fprintf( stderr, "Unexpected BUFFER (pool %u slot %u fd %d)\n", msg.pool_id, msg.slot, fd );
					if ( fd >= 0 )
						close( fd );
					break;
				}
				struct buffer *buffer = &client.buffers[msg.slot];
				if ( buffer->fd >= 0 )
					close( buffer->fd );
				buffer->fd = fd;
				buffer->num_planes = msg.num_planes;
				memcpy( buffer->offsets, msg.offsets, sizeof( buffer->offsets ) );
				memcpy( buffer->strides, msg.strides, sizeof( buffer->strides ) );
				buffer->size = msg.allocation_size;
				fd = -1;
				break;
			}

			case HERMES_SERVER_FRAME:
			{
				struct hermes_server_frame msg;
				memcpy( &msg, data, sizeof( msg ) );

				const uint64_t wait_start = now_ns();
				if ( fd >= 0 )
				{
					struct pollfd pfd = { .fd = fd, .events = POLLIN };
					if ( poll( &pfd, 1, 1000 ) != 1 )
						fprintf( stderr, "Frame %" PRIu64 ": fence did not signal within 1s\n", msg.frame_id );
					close( fd );
					fd = -1;
				}
				const uint64_t ready = now_ns();

				if ( client.last_frame_id && msg.frame_id != client.last_frame_id + 1 )
					client.dropped_ids += msg.frame_id - client.last_frame_id - 1;
				client.last_frame_id = msg.frame_id;

				client.frames++;
				client.frames_in_second++;
				client.fence_wait_ms += ( ready - wait_start ) / 1e6;
				client.latency_ms += ( ready - msg.submit_time_ns ) / 1e6;

				if ( client.frames == 1 )
				{
					printf( "First frame: pool %u slot %u, %s%s%s\n", msg.pool_id, msg.slot,
						msg.transfer == HERMES_TRANSFER_PQ ? "PQ/BT.2020" : "gamma 2.2/BT.709",
						( msg.flags & HERMES_FRAME_FLAG_FENCE ) ? ", fenced" : ", unfenced",
						( msg.flags & HERMES_FRAME_FLAG_HDR ) ? ", HDR metadata" : "" );
				}
				if ( msg.flags & HERMES_FRAME_FLAG_HDR && client.frames == 1 )
				{
					printf( "  HDR: max CLL %u, max FALL %u, mastering %u/%.4f cd/m2\n", msg.hdr_max_cll, msg.hdr_max_fall,
						msg.hdr_max_display_mastering_luminance, msg.hdr_min_display_mastering_luminance / 10000.0 );
				}

				if ( dump_path && client.frames == dump_frame_number && msg.pool_id == client.pool_id )
					dump_frame( &client, msg.slot, dump_path );

				struct hermes_client_release release = {
					.hdr = { .type = HERMES_CLIENT_RELEASE, .size = sizeof( release ) },
					.pool_id = msg.pool_id,
					.slot = msg.slot,
				};
				if ( !send_message( &client, &release, sizeof( release ) ) )
				{
					status = 1;
					goto out;
				}

				if ( text && !text_sent )
				{
					text_sent = send_text( &client, text );
					printf( "Typed \"%s\"\n", text );
				}

				const uint64_t now = now_ns();
				if ( now - client.second_start_ns >= 1000000000ull )
				{
					printf( "%.1f fps, fence wait %.3f ms, submit-to-ready %.3f ms, %" PRIu64 " frames, %" PRIu64 " gaps\n",
						client.frames_in_second * 1e9 / ( now - client.second_start_ns ),
						client.fence_wait_ms / client.frames_in_second, client.latency_ms / client.frames_in_second,
						client.frames, client.dropped_ids );
					client.frames_in_second = 0;
					client.fence_wait_ms = 0.0;
					client.latency_ms = 0.0;
					client.second_start_ns = now;
				}

				if ( max_frames && client.frames >= max_frames )
					goto out;
				break;
			}

			default:
				break;
		}

		if ( fd >= 0 )
			close( fd );
	}

out:
	close_buffers( &client );
	close( client.fd );
	return status;
}
