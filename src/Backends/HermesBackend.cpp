// Gamescope `--backend hermes`: a headless output for the Hermes streaming host.
//
// Instead of scanning out on a KMS device, every frame is composited straight
// into a small pool of dma-bufs shared with one client over a Unix socket, and
// that client's keyboard, mouse, touch and text input is injected straight into
// wlserver. Nothing here opens a DRM primary node, takes a seat, talks to logind
// or creates uinput devices, so the session runs beside the host desktop on the
// render node alone and cannot touch the host's displays or input.
//
// The wire format is protocol/hermes-stream-v1.h; HERMES.md describes the
// session around it.

#include "backend.h"
#include "rendervulkan.hpp"
#include "wlserver.hpp"
#include "steamcompmgr.hpp"
#include "refresh_rate.h"
#include "edid.h"
#include "ime.hpp"
#include "log.hpp"
#include "main.hpp"
#include "GamescopeVersion.h"
#include "hdmi.h"

#include "../../protocol/hermes-stream-v1.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <iterator>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/un.h>
#include <unistd.h>

extern int g_nPreferredOutputWidth;
extern int g_nPreferredOutputHeight;

extern gamescope::ConVar<bool> cv_hdr_enabled;

// The socket path from --hermes-socket, before $GAMESCOPE_HERMES_SOCKET and the default.
const char *g_pszHermesSocketPath = nullptr;

namespace gamescope
{
	static LogScope hermes_log( "hermes" );

	namespace
	{
		constexpr uint32_t k_uDefaultSlotCount = 3;
		constexpr uint16_t k_uDefaultHDRMaxNits = 1000;
		constexpr uint16_t k_uDefaultHDRMaxFALLNits = 400;

		// RGB target of the composite before it is converted to NV12. Ten bits keeps the
		// conversion from quantizing twice, and A2B10G10R10 is storage-capable everywhere.
		constexpr uint32_t k_uNV12ScratchFormat = DRM_FORMAT_ABGR2101010;

		bool IsRGBFormat( uint32_t uDrmFormat )
		{
			switch ( uDrmFormat )
			{
				case DRM_FORMAT_XRGB8888:
				case DRM_FORMAT_ARGB8888:
				case DRM_FORMAT_XBGR8888:
				case DRM_FORMAT_ABGR8888:
				case DRM_FORMAT_XRGB2101010:
				case DRM_FORMAT_ARGB2101010:
				case DRM_FORMAT_XBGR2101010:
				case DRM_FORMAT_ABGR2101010:
					return true;
				default:
					return false;
			}
		}

		bool Is10BitRGBFormat( uint32_t uDrmFormat )
		{
			switch ( uDrmFormat )
			{
				case DRM_FORMAT_XRGB2101010:
				case DRM_FORMAT_ARGB2101010:
				case DRM_FORMAT_XBGR2101010:
				case DRM_FORMAT_ABGR2101010:
					return true;
				default:
					return false;
			}
		}

		EStreamColorspace ToStreamColorspace( uint32_t uYUVColorspace )
		{
			switch ( uYUVColorspace )
			{
				case HERMES_YUV_BT601_LIMITED: return k_EStreamColorspace_BT601;
				case HERMES_YUV_BT601_FULL:    return k_EStreamColorspace_BT601_Full;
				case HERMES_YUV_BT709_FULL:    return k_EStreamColorspace_BT709_Full;
				case HERMES_YUV_BT709_LIMITED:
				default:                       return k_EStreamColorspace_BT709;
			}
		}

		uint32_t ToHermesYUVColorspace( EStreamColorspace eColorspace )
		{
			switch ( eColorspace )
			{
				case k_EStreamColorspace_BT601:      return HERMES_YUV_BT601_LIMITED;
				case k_EStreamColorspace_BT601_Full: return HERMES_YUV_BT601_FULL;
				case k_EStreamColorspace_BT709_Full: return HERMES_YUV_BT709_FULL;
				case k_EStreamColorspace_BT709:
				default:                             return HERMES_YUV_BT709_LIMITED;
			}
		}

		template <size_t N>
		void CopyString( char ( &szDest )[N], const char *pszSource )
		{
			snprintf( szDest, N, "%s", pszSource ? pszSource : "" );
		}

		template <typename T>
		T MakeMessage( uint16_t uType )
		{
			T msg{};
			msg.hdr.type = uType;
			msg.hdr.size = sizeof( T );
			return msg;
		}

		void CloseFd( int &nFd )
		{
			if ( nFd >= 0 )
			{
				close( nFd );
				nFd = -1;
			}
		}

		// One message, with at most one fd, without blocking. On false, errno says why.
		bool SendOnSocket( int nSocket, const void *pData, size_t uSize, int nFd = -1 )
		{
			iovec iov = { .iov_base = const_cast<void *>( pData ), .iov_len = uSize };
			msghdr msg{};
			msg.msg_iov = &iov;
			msg.msg_iovlen = 1;

			alignas( cmsghdr ) char control[ CMSG_SPACE( sizeof( int ) ) ];
			if ( nFd >= 0 )
			{
				memset( control, 0, sizeof( control ) );
				msg.msg_control = control;
				msg.msg_controllen = sizeof( control );

				cmsghdr *pCmsg = CMSG_FIRSTHDR( &msg );
				pCmsg->cmsg_level = SOL_SOCKET;
				pCmsg->cmsg_type = SCM_RIGHTS;
				pCmsg->cmsg_len = CMSG_LEN( sizeof( int ) );
				memcpy( CMSG_DATA( pCmsg ), &nFd, sizeof( int ) );
			}

			for ( ;; )
			{
				const ssize_t nSent = sendmsg( nSocket, &msg, MSG_DONTWAIT | MSG_NOSIGNAL );
				if ( nSent == ssize_t( uSize ) )
					return true;

				if ( nSent < 0 && errno == EINTR )
					continue;

				// SOCK_SEQPACKET sends whole messages or none; anything else is broken.
				if ( nSent >= 0 )
					errno = EMSGSIZE;
				return false;
			}
		}

		// Receives one message and closes any fds that came with it (v1 clients
		// never send fds). Returns the size, 0 on hang-up, -1 with errno on error.
		ssize_t ReceiveFromSocket( int nSocket, uint8_t *pBuffer, size_t uBufferSize, bool *pbTruncated )
		{
			alignas( cmsghdr ) char control[ CMSG_SPACE( sizeof( int ) * 8 ) ];

			iovec iov = { .iov_base = pBuffer, .iov_len = uBufferSize };
			msghdr msg{};
			msg.msg_iov = &iov;
			msg.msg_iovlen = 1;
			msg.msg_control = control;
			msg.msg_controllen = sizeof( control );

			const ssize_t nRead = recvmsg( nSocket, &msg, MSG_DONTWAIT | MSG_CMSG_CLOEXEC );
			if ( nRead < 0 )
				return nRead;

			for ( cmsghdr *pCmsg = CMSG_FIRSTHDR( &msg ); pCmsg; pCmsg = CMSG_NXTHDR( &msg, pCmsg ) )
			{
				if ( pCmsg->cmsg_level == SOL_SOCKET && pCmsg->cmsg_type == SCM_RIGHTS )
				{
					const size_t uFdCount = ( pCmsg->cmsg_len - CMSG_LEN( 0 ) ) / sizeof( int );
					for ( size_t i = 0; i < uFdCount; i++ )
					{
						int nFd;
						memcpy( &nFd, CMSG_DATA( pCmsg ) + i * sizeof( int ), sizeof( int ) );
						close( nFd );
					}
				}
			}

			*pbTruncated = !!( msg.msg_flags & MSG_TRUNC );
			return nRead;
		}

		// What the viewer asked for in its last CONFIGURE.
		struct HermesClientConfig_t
		{
			uint32_t uWidth = 0;
			uint32_t uHeight = 0;
			uint32_t uRefreshmHz = 0;
			uint32_t uFlags = 0;
			uint32_t uSlotCount = k_uDefaultSlotCount;
			uint32_t uYUVColorspace = HERMES_YUV_BT709_LIMITED;
			uint16_t uHDRMaxNits = k_uDefaultHDRMaxNits;
			uint16_t uHDRMaxFALLNits = k_uDefaultHDRMaxFALLNits;
			uint16_t uHDRMinLuminance = 0;
			std::vector<hermes_format_modifier> formats;
		};

		// How the buffers of a pool are laid out.
		enum class EPoolLayout
		{
			// Allocated with one of an explicit list of DRM modifiers.
			Modifiers,
			// Plain linear tiling, for drivers without modifier support and for NV12.
			Linear,
			// The driver's own tiling, reported as DRM_FORMAT_MOD_INVALID. Only an
			// importer on the same driver can make sense of it.
			Implicit,
		};

		struct HermesPoolPlan_t
		{
			uint32_t uDrmFormat = DRM_FORMAT_INVALID;
			EPoolLayout eLayout = EPoolLayout::Linear;
			std::vector<uint64_t> modifiers;
			uint32_t uSlotCount = k_uDefaultSlotCount;
			EStreamColorspace eYUVColorspace = k_EStreamColorspace_BT709;

			bool operator == ( const HermesPoolPlan_t &other ) const = default;
		};

		struct HermesPool_t
		{
			uint32_t uPoolId = 0;
			uint32_t uWidth = 0;
			uint32_t uHeight = 0;
			uint64_t ulModifier = DRM_FORMAT_MOD_INVALID;
			HermesPoolPlan_t plan;
			std::vector<OwningRc<CVulkanTexture>> pSlots;
			// The composite's RGB target when the pool is NV12.
			OwningRc<CVulkanTexture> pScratch;

			bool IsValid() const { return uPoolId != 0 && !pSlots.empty(); }
			bool IsNV12() const { return plan.uDrmFormat == DRM_FORMAT_NV12; }
		};
	}

	class CHermesBackend;

	class CHermesConnector final : public CBaseBackendConnector
	{
	public:
		CHermesConnector( CHermesBackend *pBackend )
			: m_pBackend{ pBackend }
		{
			m_HDRInfo.bAlwaysPatchEdid = true;
		}

		virtual ~CHermesConnector()
		{
		}

		virtual GamescopeScreenType GetScreenType() const override
		{
			return GAMESCOPE_SCREEN_TYPE_INTERNAL;
		}
		virtual GamescopePanelOrientation GetCurrentOrientation() const override
		{
			return GAMESCOPE_PANEL_ORIENTATION_0;
		}
		virtual bool SupportsHDR() const override
		{
			return GetHDRInfo().IsHDR10();
		}
		virtual bool IsHDRActive() const override
		{
			return g_bOutputHDREnabled && SupportsHDR();
		}
		virtual const BackendConnectorHDRInfo &GetHDRInfo() const override
		{
			return m_HDRInfo;
		}
		virtual bool IsVRRActive() const override
		{
			return false;
		}
		virtual std::span<const BackendMode> GetModes() const override
		{
			return std::span<const BackendMode>{};
		}
		virtual bool SupportsVRR() const override
		{
			return false;
		}
		virtual std::span<const uint8_t> GetRawEDID() const override
		{
			return std::span<const uint8_t>{ m_FakeEdid.begin(), m_FakeEdid.end() };
		}
		virtual std::span<const uint32_t> GetValidDynamicRefreshRates() const override
		{
			return std::span<const uint32_t>{};
		}

		virtual void GetNativeColorimetry(
			bool bHDR10,
			displaycolorimetry_t *displayColorimetry, EOTF *displayEOTF,
			displaycolorimetry_t *outputEncodingColorimetry, EOTF *outputEncodingEOTF ) const override
		{
			*displayColorimetry = displaycolorimetry_709;
			*displayEOTF = EOTF_Gamma22;

			if ( bHDR10 && GetHDRInfo().IsHDR10() )
			{
				*outputEncodingColorimetry = displaycolorimetry_2020;
				*outputEncodingEOTF = EOTF_PQ;
			}
			else
			{
				*outputEncodingColorimetry = displaycolorimetry_709;
				*outputEncodingEOTF = EOTF_Gamma22;
			}
		}

		virtual const char *GetName() const override
		{
			return "Hermes";
		}
		virtual const char *GetMake() const override
		{
			return "Gamescope";
		}
		virtual const char *GetModel() const override
		{
			return "Hermes Stream";
		}

		virtual int Present( const FrameInfo_t *pFrameInfo, bool bAsync ) override;

		// steamcompmgr thread only.
		bool SetHDR( bool bExpose, uint16_t uMaxNits, uint16_t uMaxFALLNits, uint16_t uMinLuminance )
		{
			BackendConnectorHDRInfo info = m_HDRInfo;
			info.bExposeHDRSupport = bExpose;
			info.eOutputEncodingEOTF = bExpose ? EOTF_PQ : EOTF_Gamma22;
			info.uMaxContentLightLevel = uMaxNits;
			info.uMaxFrameAverageLuminance = uMaxFALLNits;
			info.uMinContentLightLevel = uMinLuminance;

			const bool bChanged =
				info.bExposeHDRSupport != m_HDRInfo.bExposeHDRSupport ||
				info.uMaxContentLightLevel != m_HDRInfo.uMaxContentLightLevel ||
				info.uMaxFrameAverageLuminance != m_HDRInfo.uMaxFrameAverageLuminance ||
				info.uMinContentLightLevel != m_HDRInfo.uMinContentLightLevel;
			if ( !bChanged )
				return false;

			info.pDefaultMetadataBlob = bExpose ? CreateDefaultHDRMetadata( info ) : nullptr;
			m_HDRInfo = std::move( info );
			return true;
		}

		void UpdateEdid()
		{
			m_FakeEdid = GenerateSimpleEdid( g_nNestedWidth, g_nNestedHeight );
		}

	private:
		static std::shared_ptr<BackendBlob> CreateDefaultHDRMetadata( const BackendConnectorHDRInfo &info );

		CHermesBackend *m_pBackend = nullptr;
		BackendConnectorHDRInfo m_HDRInfo{};
		std::vector<uint8_t> m_FakeEdid;
	};

	class CHermesBackend final : public CBaseBackend
	{
	public:
		CHermesBackend()
			: m_Connector{ this }
		{
		}

		virtual ~CHermesBackend()
		{
			StopServer();

			vulkan_destroy_semaphore( m_pSyncSemaphore );
			m_pSyncSemaphore = VK_NULL_HANDLE;
		}

		virtual bool Init() override
		{
			g_nOutputWidth = g_nPreferredOutputWidth;
			g_nOutputHeight = g_nPreferredOutputHeight;
			g_nOutputRefresh = g_nNestedRefresh;

			if ( g_nOutputHeight == 0 )
			{
				if ( g_nOutputWidth != 0 )
				{
					fprintf( stderr, "Cannot specify -W without -H\n" );
					return false;
				}
				g_nOutputHeight = 720;
			}
			if ( g_nOutputWidth == 0 )
				g_nOutputWidth = g_nOutputHeight * 16 / 9;
			if ( g_nOutputRefresh == 0 )
				g_nOutputRefresh = ConvertHztomHz( 60 );

			if ( !vulkan_init( vulkan_get_instance(), VK_NULL_HANDLE ) )
				return false;

			if ( !wlsession_init() )
			{
				fprintf( stderr, "Failed to initialize Wayland session\n" );
				return false;
			}

			m_pSyncSemaphore = vulkan_create_sync_file_semaphore();
			if ( m_pSyncSemaphore == VK_NULL_HANDLE )
				hermes_log.infof( "The Vulkan driver cannot export sync_files; frames will be waited for on the CPU before they are sent." );

			m_bCanExportNV12 = vulkan_supports_linear_export( DRM_FORMAT_NV12, VK_IMAGE_USAGE_STORAGE_BIT );

			uint32_t uCaps = HERMES_SERVER_CAP_HDR10;
			if ( m_pSyncSemaphore != VK_NULL_HANDLE )
				uCaps |= HERMES_SERVER_CAP_SYNC_FILE;
			if ( g_device.supportsModifiers() )
				uCaps |= HERMES_SERVER_CAP_MODIFIERS;
			if ( m_bCanExportNV12 )
				uCaps |= HERMES_SERVER_CAP_NV12;
			m_uServerCaps = uCaps;

			struct stat renderStat;
			if ( g_device.drmRenderFd() >= 0 && fstat( g_device.drmRenderFd(), &renderStat ) == 0 )
				m_RenderDevId = renderStat.st_rdev;

			PublishOutputState();

			return StartServer();
		}

		virtual bool PostInit() override
		{
			m_pIME = create_local_ime();
			if ( m_pIME )
				m_uServerCaps |= HERMES_SERVER_CAP_TEXT;
			else
				hermes_log.errorf( "Could not create an input method; text input will be ignored." );

			m_Connector.UpdateEdid();
			HackUpdatePatchedEdid();
			return true;
		}

		virtual std::span<const char *const> GetInstanceExtensions() const override
		{
			return std::span<const char *const>{};
		}
		virtual std::span<const char *const> GetDeviceExtensions( VkPhysicalDevice pVkPhysicalDevice ) const override
		{
			return std::span<const char *const>{};
		}
		virtual VkImageLayout GetPresentLayout() const override
		{
			return VK_IMAGE_LAYOUT_GENERAL;
		}
		virtual void GetPreferredOutputFormat( uint32_t *pPrimaryPlaneFormat, uint32_t *pOverlayPlaneFormat ) const override
		{
			*pPrimaryPlaneFormat = VulkanFormatToDRM( VK_FORMAT_A2B10G10R10_UNORM_PACK32 );
			*pOverlayPlaneFormat = VulkanFormatToDRM( VK_FORMAT_B8G8R8A8_UNORM );
		}
		virtual bool ValidPhysicalDevice( VkPhysicalDevice pVkPhysicalDevice ) const override
		{
			return true;
		}

		virtual void DirtyState( bool bForce, bool bForceModeset ) override
		{
		}

		// steamcompmgr thread: apply what the viewer asked for since the last pass.
		virtual bool PollState() override
		{
			std::optional<PendingConfig_t> oPending;
			{
				std::unique_lock lock{ m_StateMutex };
				oPending = std::exchange( m_oPendingConfig, std::nullopt );
			}

			if ( !oPending || oPending->ulGeneration != m_ulClientGeneration.load() )
				return false;

			return ApplyConfig( oPending->ulGeneration, oPending->config );
		}

		virtual std::shared_ptr<BackendBlob> CreateBackendBlob( const std::type_info &type, std::span<const uint8_t> data ) override
		{
			return std::make_shared<BackendBlob>( data );
		}

		virtual OwningRc<IBackendFb> ImportDmabufToBackend( wlr_dmabuf_attributes *pDmaBuf ) override
		{
			return new CBaseBackendFb();
		}

		// Every frame is composited, so client buffers need not suit any scanout.
		virtual bool UsesModifiers() const override
		{
			return false;
		}
		virtual std::span<const uint64_t> GetSupportedModifiers( uint32_t uDrmFormat ) const override
		{
			return std::span<const uint64_t>{};
		}

		virtual IBackendConnector *GetCurrentConnector() override
		{
			return &m_Connector;
		}
		virtual IBackendConnector *GetConnector( GamescopeScreenType eScreenType ) override
		{
			if ( eScreenType == GAMESCOPE_SCREEN_TYPE_INTERNAL )
				return &m_Connector;

			return nullptr;
		}

		virtual bool SupportsPlaneHardwareCursor() const override
		{
			return false;
		}
		virtual bool SupportsTearing() const override
		{
			return false;
		}
		virtual bool UsesVulkanSwapchain() const override
		{
			return false;
		}
		virtual bool IsSessionBased() const override
		{
			return false;
		}
		virtual bool SupportsExplicitSync() const override
		{
			return true;
		}
		virtual bool IsPaused() const override
		{
			return false;
		}
		virtual bool IsVisible() const override
		{
			return true;
		}
		virtual glm::uvec2 CursorSurfaceSize( glm::uvec2 uvecSize ) const override
		{
			return uvecSize;
		}
		virtual bool HackTemporarySetDynamicRefresh( int nRefresh ) override
		{
			return false;
		}

		virtual void HackUpdatePatchedEdid() override
		{
			// Only children started after this see it, like the Wayland backend.
			setenv( "DXVK_HDR", ( cv_hdr_enabled && m_Connector.GetHDRInfo().bExposeHDRSupport ) ? "1" : "0", true );
			WritePatchedEdid( m_Connector.GetRawEDID(), m_Connector.GetHDRInfo(), false );
		}

		virtual void DumpDebugInfo() override
		{
			CBaseBackend::DumpDebugInfo();
			console_log.infof( "Hermes socket: %s", m_szSocketPath.c_str() );
			console_log.infof( "Hermes client connected: %s", m_ulClientGeneration.load() && m_nClientFdForDebug.load() >= 0 ? "true" : "false" );
			console_log.infof( "Hermes pool: id %u, %ux%u, format 0x%08x, modifier 0x%016llx, %zu slots",
				m_Pool.uPoolId, m_Pool.uWidth, m_Pool.uHeight, m_Pool.plan.uDrmFormat, (unsigned long long)m_Pool.ulModifier, m_Pool.pSlots.size() );
			console_log.infof( "Hermes frames sent: %llu, skipped for lack of a free slot: %llu",
				(unsigned long long)m_ulFramesSent.load(), (unsigned long long)m_ulFramesSkipped.load() );
		}

		int Present( const FrameInfo_t *pFrameInfo );

	protected:
		virtual void OnBackendBlobDestroyed( BackendBlob *pBlob ) override
		{
		}

	private:
		struct PendingConfig_t
		{
			uint64_t ulGeneration = 0;
			HermesClientConfig_t config;
		};

		struct SlotState_t
		{
			bool bClientOwned = false;
		};

		/////////////////////////
		// steamcompmgr thread
		/////////////////////////

		bool ApplyConfig( uint64_t ulGeneration, const HermesClientConfig_t &config );
		std::optional<HermesPoolPlan_t> ChoosePlan( const HermesClientConfig_t &config, bool bRequire10Bit ) const;
		bool EnsurePool( uint32_t uWidth, uint32_t uHeight );
		bool AnnouncePool( uint64_t ulGeneration );
		void SendState( uint64_t ulGeneration, bool bHDRActive );
		void PublishOutputState();

		std::optional<uint32_t> AcquireSlot();
		void ReleaseSlot( uint32_t uPoolId, uint32_t uSlot );

		/////////////////////////
		// IO thread
		/////////////////////////

		bool StartServer();
		void StopServer();
		void IOThreadFunc();
		void AcceptClient();
		void ReadPending();
		void ClosePending( const char *pszReason );
		void ReadClient();
		void DropClient( const char *pszReason );
		bool HandleMessage( const uint8_t *pData, size_t uSize );
		void HandleConfigure( const hermes_client_configure *pMsg, size_t uSize );
		void HandleRelease( const hermes_client_release *pMsg );
		void HandleInput( uint16_t uType, const uint8_t *pData, size_t uSize );
		void ReleaseHeldInput();
		void SendError( uint64_t ulGeneration, uint32_t uCode, bool bFatal, const char *pszMessage );

		// Any thread. False when the message was not sent to that connection.
		bool SendToClient( uint64_t ulGeneration, const void *pData, size_t uSize, int nFd = -1 );

		CHermesConnector m_Connector;

		VkSemaphore m_pSyncSemaphore = VK_NULL_HANDLE;
		bool m_bCanExportNV12 = false;
		dev_t m_RenderDevId = 0;
		wlserver_input_method *m_pIME = nullptr;
		// HERMES_SERVER_CAP_*, read by the IO thread for every HELLO.
		std::atomic<uint32_t> m_uServerCaps{ 0 };

		// Output state the IO thread may report while the steamcompmgr thread changes it.
		std::atomic<uint32_t> m_uPublishedWidth{ 0 };
		std::atomic<uint32_t> m_uPublishedHeight{ 0 };
		std::atomic<uint32_t> m_uPublishedRefresh{ 0 };

		// steamcompmgr thread only.
		uint64_t m_ulConfiguredGeneration = 0;
		std::optional<HermesPoolPlan_t> m_oPlan;
		HermesPool_t m_Pool;
		uint32_t m_uLastPoolId = 0;
		uint64_t m_ulAnnouncedGeneration = 0;
		uint64_t m_ulStateGeneration = 0;
		bool m_bStateHDRActive = false;
		uint64_t m_ulFrameId = 0;

		// Guards the pending config and slot ownership.
		std::mutex m_StateMutex;
		std::optional<PendingConfig_t> m_oPendingConfig;
		uint32_t m_uCurrentPoolId = 0;
		std::array<SlotState_t, HERMES_MAX_SLOTS> m_SlotStates{};

		std::atomic<bool> m_bFrameSkipped{ false };
		std::atomic<bool> m_bFrameRequested{ false };
		std::atomic<uint64_t> m_ulFramesSent{ 0 };
		std::atomic<uint64_t> m_ulFramesSkipped{ 0 };

		// Guards m_nClientFd and every send. The generation changes whenever the
		// connection does, so work aimed at an old connection never reaches a new one.
		std::mutex m_ClientMutex;
		int m_nClientFd = -1;
		std::atomic<uint64_t> m_ulClientGeneration{ 0 };
		uint64_t m_ulLastGeneration = 0;
		std::atomic<int> m_nClientFdForDebug{ -1 };

		// IO thread only.
		std::string m_szSocketPath;
		int m_nListenFd = -1;
		int m_nWakeFd = -1;
		std::thread m_IOThread;
		// A connection that has not sent its HELLO yet. It takes over from the
		// current client only then, so a stray connect cannot end a stream.
		int m_nPendingFd = -1;
		uint32_t m_uInputSequence = 0;
		std::set<uint32_t> m_HeldKeys;
		std::set<uint32_t> m_HeldButtons;
		std::set<int32_t> m_HeldTouches;
	};

	/////////////////////////
	// CHermesConnector
	/////////////////////////

	std::shared_ptr<BackendBlob> CHermesConnector::CreateDefaultHDRMetadata( const BackendConnectorHDRInfo &info )
	{
		// Same shape as the DRM backend's default infoframe: the viewer's frame
		// average for the light levels, so its display does not tone map SDR-bright
		// content down. Games that send their own metadata replace this per frame.
		hdr_output_metadata metadata{};
		hdr_metadata_infoframe *pInfoframe = &metadata.hdmi_metadata_type1;

		const uint16_t uDefaultLuminance = info.uMaxFrameAverageLuminance;

		pInfoframe->display_primaries[0].x = color_xy_to_u16( displaycolorimetry_2020.primaries.r.x );
		pInfoframe->display_primaries[0].y = color_xy_to_u16( displaycolorimetry_2020.primaries.r.y );
		pInfoframe->display_primaries[1].x = color_xy_to_u16( displaycolorimetry_2020.primaries.g.x );
		pInfoframe->display_primaries[1].y = color_xy_to_u16( displaycolorimetry_2020.primaries.g.y );
		pInfoframe->display_primaries[2].x = color_xy_to_u16( displaycolorimetry_2020.primaries.b.x );
		pInfoframe->display_primaries[2].y = color_xy_to_u16( displaycolorimetry_2020.primaries.b.y );
		pInfoframe->white_point.x = color_xy_to_u16( displaycolorimetry_2020.white.x );
		pInfoframe->white_point.y = color_xy_to_u16( displaycolorimetry_2020.white.y );
		pInfoframe->max_display_mastering_luminance = info.uMaxContentLightLevel;
		pInfoframe->min_display_mastering_luminance = info.uMinContentLightLevel;
		pInfoframe->max_cll = uDefaultLuminance;
		pInfoframe->max_fall = uDefaultLuminance;
		pInfoframe->eotf = HDMI_EOTF_ST2084;

		return std::make_shared<BackendBlob>( std::span<const uint8_t>( reinterpret_cast<const uint8_t *>( &metadata ), sizeof( metadata ) ) );
	}

	int CHermesConnector::Present( const FrameInfo_t *pFrameInfo, bool bAsync )
	{
		return m_pBackend->Present( pFrameInfo );
	}

	/////////////////////////
	// CHermesBackend: frames
	/////////////////////////

	int CHermesBackend::Present( const FrameInfo_t *pFrameInfo )
	{
		const uint64_t ulGeneration = m_ulClientGeneration.load();

		// Nobody to stream to: keep the session running, skip the work.
		if ( !ulGeneration || m_ulConfiguredGeneration != ulGeneration || !m_oPlan )
			return 0;

		if ( !EnsurePool( currentOutputWidth, currentOutputHeight ) )
			return 0;

		if ( m_ulAnnouncedGeneration != ulGeneration )
		{
			if ( !AnnouncePool( ulGeneration ) )
				return 0;
			m_ulAnnouncedGeneration = ulGeneration;
		}

		// With colour management off (or bypassed) the output is SDR whatever was asked.
		const bool bPQ = pFrameInfo->applyOutputColorMgmt && pFrameInfo->outputEncodingEOTF == EOTF_PQ;
		if ( m_ulStateGeneration != ulGeneration || bPQ != m_bStateHDRActive )
			SendState( ulGeneration, bPQ );

		std::optional<uint32_t> oSlot = AcquireSlot();
		if ( !oSlot )
		{
			// The viewer holds every slot. Skip; its next RELEASE asks for a repaint.
			m_bFrameSkipped = true;
			m_ulFramesSkipped++;
			return 0;
		}

		const uint32_t uSlot = *oSlot;
		const uint32_t uPoolId = m_Pool.uPoolId;
		gamescope::Rc<CVulkanTexture> pTarget = m_Pool.pSlots[ uSlot ].get();

		FrameInfo_t frameInfo = *pFrameInfo;

		std::optional<uint64_t> oSequence = m_Pool.IsNV12()
			? vulkan_composite( &frameInfo, pTarget, false, m_Pool.pScratch.get(), true, nullptr, m_pSyncSemaphore )
			: vulkan_composite( &frameInfo, nullptr, false, pTarget, true, nullptr, m_pSyncSemaphore );

		const uint64_t ulSubmitTime = get_time_in_nanos();

		if ( !oSequence )
		{
			hermes_log.errorf( "Composite into slot %u failed", uSlot );
			ReleaseSlot( uPoolId, uSlot );
			return 0;
		}

		int nFence = -1;
		if ( m_pSyncSemaphore != VK_NULL_HANDLE )
		{
			std::optional<int> oFence = vulkan_export_sync_file( m_pSyncSemaphore );
			if ( oFence )
			{
				nFence = *oFence;
			}
			else
			{
				// The semaphore may still hold this signal, so it cannot be signalled
				// again. Let the work finish, then drop to CPU waits for good.
				vulkan_wait( *oSequence, false );
				vulkan_destroy_semaphore( m_pSyncSemaphore );
				m_pSyncSemaphore = VK_NULL_HANDLE;
				m_uServerCaps &= ~uint32_t( HERMES_SERVER_CAP_SYNC_FILE );
				hermes_log.errorf( "sync_file export failed; frames will be waited for on the CPU from now on." );
			}
		}
		else
		{
			vulkan_wait( *oSequence, false );
		}

		hermes_server_frame msg = MakeMessage<hermes_server_frame>( HERMES_SERVER_FRAME );
		msg.pool_id = uPoolId;
		msg.slot = uSlot;
		msg.frame_id = ++m_ulFrameId;
		msg.submit_time_ns = ulSubmitTime;
		msg.flags = 0;
		msg.transfer = bPQ ? HERMES_TRANSFER_PQ : HERMES_TRANSFER_GAMMA22;
		msg.primaries = bPQ ? HERMES_PRIMARIES_BT2020 : HERMES_PRIMARIES_BT709;
		msg.yuv_colorspace = m_Pool.IsNV12() ? ToHermesYUVColorspace( m_Pool.plan.eYUVColorspace ) : 0;

		if ( nFence >= 0 )
			msg.flags |= HERMES_FRAME_FLAG_FENCE;

		if ( m_bFrameRequested.exchange( false ) )
			msg.flags |= HERMES_FRAME_FLAG_REQUESTED;

		if ( bPQ )
		{
			// The game's own metadata when it gave any, the viewer's display otherwise.
			std::shared_ptr<BackendBlob> pMetadataBlob = m_Connector.GetHDRInfo().pDefaultMetadataBlob;
			if ( wlserver_vk_swapchain_feedback *pFeedback = steamcompmgr_get_base_layer_swapchain_feedback() )
			{
				if ( pFeedback->hdr_metadata_blob )
					pMetadataBlob = pFeedback->hdr_metadata_blob;
			}

			if ( pMetadataBlob && pMetadataBlob->GetData().size() == sizeof( hdr_output_metadata ) )
			{
				const hdr_metadata_infoframe &infoframe = pMetadataBlob->View<hdr_output_metadata>().hdmi_metadata_type1;
				for ( int i = 0; i < 3; i++ )
				{
					msg.hdr_display_primaries_x[i] = infoframe.display_primaries[i].x;
					msg.hdr_display_primaries_y[i] = infoframe.display_primaries[i].y;
				}
				msg.hdr_white_point_x = infoframe.white_point.x;
				msg.hdr_white_point_y = infoframe.white_point.y;
				msg.hdr_max_display_mastering_luminance = infoframe.max_display_mastering_luminance;
				msg.hdr_min_display_mastering_luminance = infoframe.min_display_mastering_luminance;
				msg.hdr_max_cll = infoframe.max_cll;
				msg.hdr_max_fall = infoframe.max_fall;
				msg.flags |= HERMES_FRAME_FLAG_HDR;
			}
		}

		const bool bSent = SendToClient( ulGeneration, &msg, sizeof( msg ), nFence );
		CloseFd( nFence );

		if ( bSent )
			m_ulFramesSent++;
		else
			ReleaseSlot( uPoolId, uSlot );

		return 0;
	}

	std::optional<uint32_t> CHermesBackend::AcquireSlot()
	{
		std::unique_lock lock{ m_StateMutex };
		if ( m_uCurrentPoolId != m_Pool.uPoolId )
			return std::nullopt;

		// Lowest free slot; the viewer releases in order, so this cycles through them.
		for ( uint32_t i = 0; i < m_Pool.pSlots.size(); i++ )
		{
			if ( !m_SlotStates[i].bClientOwned )
			{
				m_SlotStates[i].bClientOwned = true;
				return i;
			}
		}

		return std::nullopt;
	}

	void CHermesBackend::ReleaseSlot( uint32_t uPoolId, uint32_t uSlot )
	{
		std::unique_lock lock{ m_StateMutex };
		if ( uPoolId == m_uCurrentPoolId && uSlot < m_SlotStates.size() )
			m_SlotStates[ uSlot ].bClientOwned = false;
	}

	std::optional<HermesPoolPlan_t> CHermesBackend::ChoosePlan( const HermesClientConfig_t &config, bool bRequire10Bit ) const
	{
		// Group the viewer's modifiers by format, keeping its order of preference.
		std::vector<std::pair<uint32_t, std::vector<uint64_t>>> formats;
		for ( const hermes_format_modifier &entry : config.formats )
		{
			auto iter = std::find_if( formats.begin(), formats.end(), [&]( const auto &pair ) { return pair.first == entry.drm_format; } );
			if ( iter == formats.end() )
			{
				formats.emplace_back( entry.drm_format, std::vector<uint64_t>{} );
				iter = std::prev( formats.end() );
			}
			iter->second.push_back( entry.modifier );
		}

		for ( const auto &[ uDrmFormat, modifiers ] : formats )
		{
			const bool bAcceptsLinear = Algorithm::Contains( modifiers, DRM_FORMAT_MOD_LINEAR );
			const bool bAcceptsImplicit = Algorithm::Contains( modifiers, DRM_FORMAT_MOD_INVALID );

			HermesPoolPlan_t plan;
			plan.uDrmFormat = uDrmFormat;
			plan.uSlotCount = config.uSlotCount;
			plan.eYUVColorspace = ToStreamColorspace( config.uYUVColorspace );

			if ( uDrmFormat == DRM_FORMAT_NV12 )
			{
				// The NV12 path writes per-plane views of a linear image, as captures do.
				if ( bRequire10Bit || !m_bCanExportNV12 || !( bAcceptsLinear || bAcceptsImplicit ) )
					continue;

				plan.eLayout = EPoolLayout::Linear;
				return plan;
			}

			if ( !IsRGBFormat( uDrmFormat ) || ( bRequire10Bit && !Is10BitRGBFormat( uDrmFormat ) ) )
				continue;

			std::vector<uint64_t> explicitModifiers;
			for ( uint64_t ulModifier : modifiers )
			{
				if ( ulModifier != DRM_FORMAT_MOD_INVALID )
					explicitModifiers.push_back( ulModifier );
			}

			if ( !explicitModifiers.empty() )
			{
				std::vector<uint64_t> exportable = vulkan_get_exportable_modifiers( uDrmFormat, VK_IMAGE_USAGE_STORAGE_BIT, explicitModifiers );
				if ( !exportable.empty() )
				{
					plan.eLayout = EPoolLayout::Modifiers;
					plan.modifiers = std::move( exportable );
					return plan;
				}
			}

			// Without modifier support a linear image is still a linear image.
			if ( bAcceptsLinear && !g_device.supportsModifiers() && vulkan_supports_linear_export( uDrmFormat, VK_IMAGE_USAGE_STORAGE_BIT ) )
			{
				plan.eLayout = EPoolLayout::Linear;
				return plan;
			}

			if ( bAcceptsImplicit )
			{
				plan.eLayout = EPoolLayout::Implicit;
				return plan;
			}
		}

		return std::nullopt;
	}

	bool CHermesBackend::ApplyConfig( uint64_t ulGeneration, const HermesClientConfig_t &config )
	{
		bool bModeChanged = false;

		if ( config.uWidth && config.uHeight && ( config.uWidth != g_nOutputWidth || config.uHeight != g_nOutputHeight ) )
		{
			g_nOutputWidth = config.uWidth;
			g_nOutputHeight = config.uHeight;
			bModeChanged = true;
		}

		if ( config.uRefreshmHz && int( config.uRefreshmHz ) != g_nOutputRefresh )
		{
			g_nOutputRefresh = int( config.uRefreshmHz );
			// The vblank timer runs at the nested rate whenever one is set.
			if ( g_nNestedRefresh )
				g_nNestedRefresh = int( config.uRefreshmHz );
			bModeChanged = true;
		}

		bool bNestedChanged = false;
		if ( ( config.uFlags & HERMES_CONFIGURE_RESIZE_NESTED ) &&
			 ( g_nNestedWidth != int( g_nOutputWidth ) || g_nNestedHeight != int( g_nOutputHeight ) ) )
		{
			g_nNestedWidth = int( g_nOutputWidth );
			g_nNestedHeight = int( g_nOutputHeight );
			bNestedChanged = true;
		}

		if ( bModeChanged || bNestedChanged )
		{
			wlserver_lock();
			for ( int i = 0; i < g_nXWaylandCount; i++ )
				wlserver_set_xwayland_server_mode( i, g_nNestedWidth, g_nNestedHeight, g_nOutputRefresh );
			wlserver_unlock();
		}

		// HDR needs a 10-bit RGB format to carry PQ; without one it is not offered at all.
		std::optional<HermesPoolPlan_t> oPlan;
		bool bExposeHDR = false;
		if ( config.uFlags & HERMES_CONFIGURE_HDR )
		{
			oPlan = ChoosePlan( config, true );
			bExposeHDR = oPlan.has_value();
			if ( !bExposeHDR )
				SendError( ulGeneration, HERMES_ERROR_NO_FORMAT, false, "HDR needs a 10-bit RGB format this GPU can export; continuing in SDR" );
		}
		if ( !oPlan )
			oPlan = ChoosePlan( config, false );

		const bool bHDRChanged = m_Connector.SetHDR( bExposeHDR, config.uHDRMaxNits, config.uHDRMaxFALLNits, config.uHDRMinLuminance );
		if ( bNestedChanged || bHDRChanged )
		{
			m_Connector.UpdateEdid();
			HackUpdatePatchedEdid();
		}

		if ( bExposeHDR && !cv_hdr_enabled )
			hermes_log.infof( "HDR is offered to games, but frames stay SDR until HDR is enabled (--hdr-enabled or Steam's setting)." );

		m_ulConfiguredGeneration = ulGeneration;

		if ( !oPlan )
		{
			m_oPlan = std::nullopt;
			SendError( ulGeneration, HERMES_ERROR_NO_FORMAT, false, "No format in CONFIGURE can be produced on this GPU" );
		}
		else if ( !m_oPlan || !( *m_oPlan == *oPlan ) )
		{
			hermes_log.infof( "Stream format 0x%08x, %s layout, %u slots",
				oPlan->uDrmFormat,
				oPlan->eLayout == EPoolLayout::Modifiers ? "modifier" : oPlan->eLayout == EPoolLayout::Linear ? "linear" : "implicit",
				oPlan->uSlotCount );
			m_oPlan = std::move( oPlan );
		}

		// A new plan or size makes EnsurePool rebuild; the viewer needs the state either way.
		m_ulStateGeneration = 0;
		PublishOutputState();

		// Make sure there is a frame to send even if nothing on screen changes.
		m_bFrameRequested = true;
		force_repaint();

		return bModeChanged || bNestedChanged || bHDRChanged;
	}

	bool CHermesBackend::EnsurePool( uint32_t uWidth, uint32_t uHeight )
	{
		if ( m_Pool.IsValid() && m_Pool.uWidth == uWidth && m_Pool.uHeight == uHeight && m_Pool.plan == *m_oPlan )
			return true;

		const HermesPoolPlan_t &plan = *m_oPlan;

		HermesPool_t pool;
		pool.uPoolId = ++m_uLastPoolId;
		pool.uWidth = uWidth;
		pool.uHeight = uHeight;
		pool.plan = plan;

		// The first slot settles the modifier, the rest are made to match it.
		std::vector<uint64_t> modifiers = plan.modifiers;

		for ( uint32_t i = 0; i < plan.uSlotCount; i++ )
		{
			CVulkanTexture::createFlags flags;
			flags.bStorage = true;
			flags.bExportable = true;
			flags.bStrictExport = true;

			switch ( plan.eLayout )
			{
				case EPoolLayout::Modifiers:
					flags.pExportModifiers = modifiers.data();
					flags.uExportModifierCount = uint32_t( modifiers.size() );
					break;
				case EPoolLayout::Linear:
					flags.bLinear = true;
					break;
				case EPoolLayout::Implicit:
					break;
			}

			OwningRc<CVulkanTexture> pTexture = new CVulkanTexture();
			if ( !pTexture->BInit( uWidth, uHeight, 1u, plan.uDrmFormat, flags ) || pTexture->dmabuf().fd[0] < 0 )
			{
				hermes_log.errorf( "Failed to allocate stream buffer %u (%ux%u, format 0x%08x)", i, uWidth, uHeight, plan.uDrmFormat );
				SendError( m_ulClientGeneration.load(), HERMES_ERROR_ALLOCATION, false, "Stream buffers could not be allocated" );
				m_oPlan = std::nullopt;
				return false;
			}

			if ( plan.uDrmFormat == DRM_FORMAT_NV12 )
				pTexture->setStreamColorspace( plan.eYUVColorspace );

			if ( i == 0 )
			{
				switch ( plan.eLayout )
				{
					case EPoolLayout::Modifiers:
						pool.ulModifier = pTexture->dmabuf().modifier;
						modifiers = { pool.ulModifier };
						break;
					case EPoolLayout::Linear:
						pool.ulModifier = DRM_FORMAT_MOD_LINEAR;
						break;
					case EPoolLayout::Implicit:
						pool.ulModifier = DRM_FORMAT_MOD_INVALID;
						break;
				}
			}

			pool.pSlots.emplace_back( std::move( pTexture ) );
		}

		if ( plan.uDrmFormat == DRM_FORMAT_NV12 )
		{
			CVulkanTexture::createFlags flags;
			flags.bStorage = true;
			flags.bSampled = true;

			pool.pScratch = new CVulkanTexture();
			if ( !pool.pScratch->BInit( uWidth, uHeight, 1u, k_uNV12ScratchFormat, flags ) )
			{
				hermes_log.errorf( "Failed to allocate the NV12 composite target (%ux%u)", uWidth, uHeight );
				SendError( m_ulClientGeneration.load(), HERMES_ERROR_ALLOCATION, false, "Stream buffers could not be allocated" );
				m_oPlan = std::nullopt;
				return false;
			}
		}

		hermes_log.infof( "Stream pool %u: %ux%u, format 0x%08x, modifier 0x%016llx, %u slots",
			pool.uPoolId, uWidth, uHeight, plan.uDrmFormat, (unsigned long long)pool.ulModifier, plan.uSlotCount );

		{
			std::unique_lock lock{ m_StateMutex };
			m_uCurrentPoolId = pool.uPoolId;
			m_SlotStates = {};
		}

		// The old buffers live on until in-flight work and the viewer's imports let go.
		m_Pool = std::move( pool );
		m_ulAnnouncedGeneration = 0;
		return true;
	}

	bool CHermesBackend::AnnouncePool( uint64_t ulGeneration )
	{
		hermes_server_pool poolMsg = MakeMessage<hermes_server_pool>( HERMES_SERVER_POOL );
		poolMsg.pool_id = m_Pool.uPoolId;
		poolMsg.slot_count = uint32_t( m_Pool.pSlots.size() );
		poolMsg.width = m_Pool.uWidth;
		poolMsg.height = m_Pool.uHeight;
		poolMsg.drm_format = m_Pool.plan.uDrmFormat;
		poolMsg.flags = HERMES_POOL_SINGLE_ALLOCATION;
		poolMsg.modifier = m_Pool.ulModifier;

		if ( !SendToClient( ulGeneration, &poolMsg, sizeof( poolMsg ) ) )
			return false;

		for ( uint32_t i = 0; i < m_Pool.pSlots.size(); i++ )
		{
			CVulkanTexture *pTexture = m_Pool.pSlots[i].get();

			hermes_server_buffer bufferMsg = MakeMessage<hermes_server_buffer>( HERMES_SERVER_BUFFER );
			bufferMsg.pool_id = m_Pool.uPoolId;
			bufferMsg.slot = i;

			uint32_t uPlaneCount = 0;
			if ( !vulkan_get_plane_layouts( pTexture, &uPlaneCount, bufferMsg.offsets, bufferMsg.strides ) )
			{
				hermes_log.errorf( "No plane layout for stream buffer %u", i );
				return false;
			}
			bufferMsg.num_planes = uPlaneCount;

			const int nFd = pTexture->dmabuf().fd[0];
			const off_t ulSize = lseek( nFd, 0, SEEK_END );
			bufferMsg.allocation_size = ulSize > 0 ? uint64_t( ulSize ) : 0;

			if ( !SendToClient( ulGeneration, &bufferMsg, sizeof( bufferMsg ), nFd ) )
				return false;
		}

		return true;
	}

	void CHermesBackend::SendState( uint64_t ulGeneration, bool bHDRActive )
	{
		hermes_server_state msg = MakeMessage<hermes_server_state>( HERMES_SERVER_STATE );
		msg.width = g_nOutputWidth;
		msg.height = g_nOutputHeight;
		msg.refresh_mhz = uint32_t( g_nOutputRefresh );
		msg.nested_width = uint32_t( g_nNestedWidth );
		msg.nested_height = uint32_t( g_nNestedHeight );
		if ( bHDRActive )
			msg.flags |= HERMES_STATE_HDR_ACTIVE;
		if ( m_Connector.SupportsHDR() && cv_hdr_enabled )
			msg.flags |= HERMES_STATE_HDR_CAPABLE;

		if ( SendToClient( ulGeneration, &msg, sizeof( msg ) ) )
		{
			m_ulStateGeneration = ulGeneration;
			m_bStateHDRActive = bHDRActive;
		}
	}

	void CHermesBackend::PublishOutputState()
	{
		m_uPublishedWidth = g_nOutputWidth;
		m_uPublishedHeight = g_nOutputHeight;
		m_uPublishedRefresh = uint32_t( g_nOutputRefresh );
	}

	/////////////////////////
	// CHermesBackend: socket
	/////////////////////////

	bool CHermesBackend::StartServer()
	{
		const char *pszPath = g_pszHermesSocketPath;
		if ( !pszPath || !*pszPath )
			pszPath = getenv( "GAMESCOPE_HERMES_SOCKET" );

		if ( pszPath && *pszPath )
		{
			m_szSocketPath = pszPath;
		}
		else
		{
			const char *pszRuntimeDir = getenv( "XDG_RUNTIME_DIR" );
			if ( !pszRuntimeDir || !*pszRuntimeDir )
			{
				hermes_log.errorf( "XDG_RUNTIME_DIR is not set; pass --hermes-socket" );
				return false;
			}

			char szPath[ PATH_MAX ];
			snprintf( szPath, sizeof( szPath ), "%s/gamescope-hermes-%d", pszRuntimeDir, int( getpid() ) );
			m_szSocketPath = szPath;
		}

		sockaddr_un addr{};
		addr.sun_family = AF_UNIX;
		if ( m_szSocketPath.size() >= sizeof( addr.sun_path ) )
		{
			hermes_log.errorf( "Socket path is too long: %s", m_szSocketPath.c_str() );
			return false;
		}
		memcpy( addr.sun_path, m_szSocketPath.c_str(), m_szSocketPath.size() + 1 );

		m_nListenFd = socket( AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0 );
		if ( m_nListenFd < 0 )
		{
			hermes_log.errorf_errno( "socket failed" );
			return false;
		}

		// A socket left behind by a gamescope that died is fine to take over;
		// one somebody still listens on is not.
		struct stat pathStat;
		if ( lstat( m_szSocketPath.c_str(), &pathStat ) == 0 )
		{
			if ( !S_ISSOCK( pathStat.st_mode ) )
			{
				hermes_log.errorf( "%s exists and is not a socket", m_szSocketPath.c_str() );
				CloseFd( m_nListenFd );
				return false;
			}

			int nProbeFd = socket( AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0 );
			const bool bLive = nProbeFd >= 0 && connect( nProbeFd, reinterpret_cast<const sockaddr *>( &addr ), sizeof( addr ) ) == 0;
			CloseFd( nProbeFd );
			if ( bLive )
			{
				hermes_log.errorf( "Another Gamescope is already serving %s", m_szSocketPath.c_str() );
				CloseFd( m_nListenFd );
				return false;
			}

			unlink( m_szSocketPath.c_str() );
		}

		// Only this user may stream the session or drive its input.
		const mode_t oldMask = umask( 0077 );
		const int nBindRet = bind( m_nListenFd, reinterpret_cast<const sockaddr *>( &addr ), sizeof( addr ) );
		umask( oldMask );
		if ( nBindRet != 0 )
		{
			hermes_log.errorf_errno( "bind to %s failed", m_szSocketPath.c_str() );
			CloseFd( m_nListenFd );
			return false;
		}

		if ( listen( m_nListenFd, 4 ) != 0 )
		{
			hermes_log.errorf_errno( "listen failed" );
			unlink( m_szSocketPath.c_str() );
			CloseFd( m_nListenFd );
			return false;
		}

		m_nWakeFd = eventfd( 0, EFD_CLOEXEC | EFD_NONBLOCK );
		if ( m_nWakeFd < 0 )
		{
			hermes_log.errorf_errno( "eventfd failed" );
			unlink( m_szSocketPath.c_str() );
			CloseFd( m_nListenFd );
			return false;
		}

		// Children (and whoever inspects the session) can find the socket here.
		setenv( "GAMESCOPE_HERMES_SOCKET", m_szSocketPath.c_str(), 1 );

		hermes_log.infof( "Listening for a Hermes client on %s", m_szSocketPath.c_str() );

		m_IOThread = std::thread( [this]() { IOThreadFunc(); } );
		return true;
	}

	void CHermesBackend::StopServer()
	{
		if ( m_IOThread.joinable() )
		{
			const uint64_t ulOne = 1;
			if ( write( m_nWakeFd, &ulOne, sizeof( ulOne ) ) < 0 )
				hermes_log.errorf_errno( "Failed to wake the IO thread" );
			m_IOThread.join();
		}

		{
			std::unique_lock lock{ m_ClientMutex };
			CloseFd( m_nClientFd );
		}
		CloseFd( m_nPendingFd );

		if ( m_nListenFd >= 0 )
		{
			unlink( m_szSocketPath.c_str() );
			CloseFd( m_nListenFd );
		}

		CloseFd( m_nWakeFd );
	}

	void CHermesBackend::IOThreadFunc()
	{
		pthread_setname_np( pthread_self(), "gamescope-hermes" );

		for ( ;; )
		{
			int nClientFd;
			{
				std::unique_lock lock{ m_ClientMutex };
				nClientFd = m_nClientFd;
			}

			// poll() skips negative fds, so absent connections need no special case.
			pollfd fds[4] =
			{
				{ .fd = m_nWakeFd, .events = POLLIN },
				{ .fd = m_nListenFd, .events = POLLIN },
				{ .fd = nClientFd, .events = POLLIN },
				{ .fd = m_nPendingFd, .events = POLLIN },
			};

			if ( poll( fds, std::size( fds ), -1 ) < 0 )
			{
				if ( errno == EINTR )
					continue;

				hermes_log.errorf_errno( "poll failed" );
				break;
			}

			if ( fds[0].revents )
				break;

			if ( fds[2].revents & POLLIN )
				ReadClient();
			else if ( fds[2].revents & ( POLLHUP | POLLERR | POLLNVAL ) )
				DropClient( "hung up" );

			if ( fds[3].revents & POLLIN )
				ReadPending();
			else if ( fds[3].revents & ( POLLHUP | POLLERR | POLLNVAL ) )
				ClosePending( "hung up before HELLO" );

			// Last, so the pending slot polled above is still the one that was read.
			if ( fds[1].revents & POLLIN )
				AcceptClient();
		}
	}

	void CHermesBackend::AcceptClient()
	{
		for ( ;; )
		{
			int nFd = accept4( m_nListenFd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC );
			if ( nFd < 0 )
			{
				if ( errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR )
					hermes_log.errorf_errno( "accept failed" );
				return;
			}

			// Only the newest connection may take over.
			if ( m_nPendingFd >= 0 )
				ClosePending( "superseded by a newer connection" );
			m_nPendingFd = nFd;

			hermes_server_hello hello = MakeMessage<hermes_server_hello>( HERMES_SERVER_HELLO );
			hello.version_major = HERMES_STREAM_VERSION_MAJOR;
			hello.version_minor = HERMES_STREAM_VERSION_MINOR;
			hello.caps = m_uServerCaps.load();
			hello.width = m_uPublishedWidth;
			hello.height = m_uPublishedHeight;
			hello.refresh_mhz = m_uPublishedRefresh;
			hello.render_dev_major = m_RenderDevId ? major( m_RenderDevId ) : 0;
			hello.render_dev_minor = m_RenderDevId ? minor( m_RenderDevId ) : 0;
			CopyString( hello.gamescope_version, k_szGamescopeVersion );

			if ( !SendOnSocket( m_nPendingFd, &hello, sizeof( hello ) ) )
				ClosePending( "could not be sent HELLO" );
		}
	}

	void CHermesBackend::ClosePending( const char *pszReason )
	{
		if ( m_nPendingFd < 0 )
			return;

		CloseFd( m_nPendingFd );
		hermes_log.debugf( "Connection closed before it took over: %s", pszReason );
	}

	void CHermesBackend::ReadPending()
	{
		alignas( 8 ) uint8_t buffer[ HERMES_MAX_MESSAGE_SIZE ];
		bool bTruncated = false;
		const ssize_t nRead = ReceiveFromSocket( m_nPendingFd, buffer, sizeof( buffer ), &bTruncated );
		if ( nRead < 0 )
		{
			if ( errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR )
				ClosePending( strerror( errno ) );
			return;
		}
		if ( nRead == 0 )
		{
			ClosePending( "closed the connection" );
			return;
		}

		auto Reject = [&]( uint32_t uCode, const char *pszMessage )
		{
			hermes_log.errorf( "Rejected a connection: %s", pszMessage );
			hermes_server_error error = MakeMessage<hermes_server_error>( HERMES_SERVER_ERROR );
			error.code = uCode;
			error.fatal = 1;
			CopyString( error.message, pszMessage );
			SendOnSocket( m_nPendingFd, &error, sizeof( error ) );
			CloseFd( m_nPendingFd );
		};

		hermes_client_hello hello;
		if ( bTruncated || size_t( nRead ) < sizeof( hello ) )
		{
			Reject( HERMES_ERROR_PROTOCOL, "HELLO must come first" );
			return;
		}
		memcpy( &hello, buffer, sizeof( hello ) );

		if ( hello.hdr.type != HERMES_CLIENT_HELLO || hello.hdr.size != size_t( nRead ) )
		{
			Reject( HERMES_ERROR_PROTOCOL, "HELLO must come first" );
			return;
		}

		if ( hello.version_major != HERMES_STREAM_VERSION_MAJOR )
		{
			Reject( HERMES_ERROR_VERSION, "Unsupported hermes-stream major version" );
			return;
		}

		// A valid HELLO: this connection takes over the stream and the input.
		DropClient( "replaced by a new connection" );

		{
			std::unique_lock lock{ m_ClientMutex };
			m_nClientFd = std::exchange( m_nPendingFd, -1 );
			m_ulClientGeneration = ++m_ulLastGeneration;
			m_nClientFdForDebug = m_nClientFd;
		}

		char szName[ sizeof( hello.client_name ) + 1 ] = {};
		memcpy( szName, hello.client_name, sizeof( hello.client_name ) );
		hermes_log.infof( "Hermes client \"%s\" connected (hermes-stream %u.%u)", szName, hello.version_major, hello.version_minor );

		// It may have sent CONFIGURE right behind its HELLO.
		ReadClient();
	}

	void CHermesBackend::DropClient( const char *pszReason )
	{
		{
			std::unique_lock lock{ m_ClientMutex };
			if ( m_nClientFd < 0 )
				return;

			CloseFd( m_nClientFd );
			// A fresh generation strands any work still aimed at this connection.
			m_ulClientGeneration = ++m_ulLastGeneration;
			m_nClientFdForDebug = -1;
		}

		{
			std::unique_lock lock{ m_StateMutex };
			m_oPendingConfig = std::nullopt;
			m_SlotStates = {};
		}

		m_bFrameSkipped = false;

		ReleaseHeldInput();

		hermes_log.infof( "Hermes client disconnected: %s", pszReason );
	}

	void CHermesBackend::ReadClient()
	{
		for ( ;; )
		{
			alignas( 8 ) uint8_t buffer[ HERMES_MAX_MESSAGE_SIZE ];

			// Only this thread changes m_nClientFd, so it stays valid without the lock.
			int nClientFd;
			{
				std::unique_lock lock{ m_ClientMutex };
				nClientFd = m_nClientFd;
			}
			if ( nClientFd < 0 )
				return;

			bool bTruncated = false;
			const ssize_t nRead = ReceiveFromSocket( nClientFd, buffer, sizeof( buffer ), &bTruncated );
			if ( nRead < 0 )
			{
				if ( errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR )
					return;

				DropClient( strerror( errno ) );
				return;
			}

			if ( nRead == 0 )
			{
				DropClient( "closed the connection" );
				return;
			}

			if ( bTruncated )
			{
				SendError( m_ulClientGeneration.load(), HERMES_ERROR_PROTOCOL, true, "Message larger than HERMES_MAX_MESSAGE_SIZE" );
				DropClient( "sent an oversized message" );
				return;
			}

			if ( !HandleMessage( buffer, size_t( nRead ) ) )
				return;
		}
	}

	bool CHermesBackend::HandleMessage( const uint8_t *pData, size_t uSize )
	{
		const uint64_t ulGeneration = m_ulClientGeneration.load();

		hermes_msg_header header;
		if ( uSize < sizeof( header ) )
		{
			SendError( ulGeneration, HERMES_ERROR_PROTOCOL, true, "Message shorter than its header" );
			DropClient( "sent a truncated message" );
			return false;
		}
		memcpy( &header, pData, sizeof( header ) );

		if ( header.size != uSize )
		{
			SendError( ulGeneration, HERMES_ERROR_PROTOCOL, true, "Header size does not match the message" );
			DropClient( "sent a message of the wrong size" );
			return false;
		}

		auto Expect = [&]( size_t uMinSize ) -> bool
		{
			if ( uSize >= uMinSize )
				return true;

			SendError( ulGeneration, HERMES_ERROR_PROTOCOL, true, "Message too short for its type" );
			DropClient( "sent a message too short for its type" );
			return false;
		};

		// ReadPending took this connection's HELLO before it became the client.
		switch ( header.type )
		{
			case HERMES_CLIENT_HELLO:
			{
				SendError( ulGeneration, HERMES_ERROR_PROTOCOL, true, "HELLO sent twice" );
				DropClient( "sent HELLO twice" );
				return false;
			}

			case HERMES_CLIENT_CONFIGURE:
			{
				if ( !Expect( sizeof( hermes_client_configure ) ) )
					return false;

				HandleConfigure( reinterpret_cast<const hermes_client_configure *>( pData ), uSize );
				return m_ulClientGeneration.load() == ulGeneration;
			}

			case HERMES_CLIENT_RELEASE:
			{
				if ( !Expect( sizeof( hermes_client_release ) ) )
					return false;

				hermes_client_release release;
				memcpy( &release, pData, sizeof( release ) );
				HandleRelease( &release );
				return true;
			}

			case HERMES_CLIENT_REQUEST_FRAME:
			{
				m_bFrameRequested = true;
				force_repaint();
				return true;
			}

			case HERMES_CLIENT_POINTER_MOTION:
			case HERMES_CLIENT_POINTER_ABSOLUTE:
			case HERMES_CLIENT_POINTER_BUTTON:
			case HERMES_CLIENT_POINTER_AXIS:
			case HERMES_CLIENT_KEY:
			case HERMES_CLIENT_TOUCH:
			case HERMES_CLIENT_TEXT:
			{
				HandleInput( header.type, pData, uSize );
				return m_ulClientGeneration.load() == ulGeneration;
			}

			default:
				// Newer minor versions may add messages; skip what we don't know.
				hermes_log.debugf( "Ignoring unknown message type %u", header.type );
				return true;
		}
	}

	void CHermesBackend::HandleConfigure( const hermes_client_configure *pMsg, size_t uSize )
	{
		const uint64_t ulGeneration = m_ulClientGeneration.load();

		hermes_client_configure configure;
		memcpy( &configure, pMsg, sizeof( configure ) );

		if ( configure.num_formats > HERMES_MAX_FORMATS ||
			 uSize != sizeof( configure ) + size_t( configure.num_formats ) * sizeof( hermes_format_modifier ) )
		{
			SendError( ulGeneration, HERMES_ERROR_PROTOCOL, true, "CONFIGURE format list does not match its size" );
			DropClient( "sent a malformed CONFIGURE" );
			return;
		}

		HermesClientConfig_t config;
		config.uWidth = configure.width;
		config.uHeight = configure.height;
		config.uRefreshmHz = configure.refresh_mhz;
		config.uFlags = configure.flags;
		config.uSlotCount = configure.slot_count ? std::clamp<uint32_t>( configure.slot_count, HERMES_MIN_SLOTS, HERMES_MAX_SLOTS ) : k_uDefaultSlotCount;
		config.uYUVColorspace = configure.yuv_colorspace ? configure.yuv_colorspace : uint32_t( HERMES_YUV_BT709_LIMITED );
		config.uHDRMaxNits = configure.hdr_max_luminance ? configure.hdr_max_luminance : k_uDefaultHDRMaxNits;
		config.uHDRMaxFALLNits = configure.hdr_max_frame_average ? configure.hdr_max_frame_average : k_uDefaultHDRMaxFALLNits;
		config.uHDRMinLuminance = configure.hdr_min_luminance;

		// Sanity limits; anything past these is a broken client, not a big screen.
		if ( config.uWidth > 16384 || config.uHeight > 16384 || ( !config.uWidth != !config.uHeight ) )
		{
			SendError( ulGeneration, HERMES_ERROR_PROTOCOL, true, "CONFIGURE size is out of range" );
			DropClient( "asked for an impossible size" );
			return;
		}

		config.formats.resize( configure.num_formats );
		if ( configure.num_formats )
			memcpy( config.formats.data(), reinterpret_cast<const uint8_t *>( pMsg ) + sizeof( configure ), configure.num_formats * sizeof( hermes_format_modifier ) );

		{
			std::unique_lock lock{ m_StateMutex };
			m_oPendingConfig = PendingConfig_t{ ulGeneration, std::move( config ) };
		}

		nudge_steamcompmgr();
	}

	void CHermesBackend::HandleRelease( const hermes_client_release *pMsg )
	{
		bool bInvalid = false;
		{
			std::unique_lock lock{ m_StateMutex };

			// Buffers of an older pool are gone already; their RELEASEs mean nothing.
			if ( pMsg->pool_id != m_uCurrentPoolId )
				return;

			if ( pMsg->slot >= m_SlotStates.size() || !m_SlotStates[ pMsg->slot ].bClientOwned )
				bInvalid = true;
			else
				m_SlotStates[ pMsg->slot ].bClientOwned = false;
		}

		if ( bInvalid )
		{
			SendError( m_ulClientGeneration.load(), HERMES_ERROR_INVALID_SLOT, false, "RELEASE of a slot the client does not own" );
			return;
		}

		// A frame waited for this slot; send the current content now.
		if ( m_bFrameSkipped.exchange( false ) )
			force_repaint();
	}

	void CHermesBackend::HandleInput( uint16_t uType, const uint8_t *pData, size_t uSize )
	{
		const uint64_t ulGeneration = m_ulClientGeneration.load();

		auto Read = [&]<typename T>( T *pOut ) -> bool
		{
			if ( uSize < sizeof( T ) )
			{
				SendError( ulGeneration, HERMES_ERROR_PROTOCOL, true, "Input message too short for its type" );
				DropClient( "sent a truncated input message" );
				return false;
			}
			memcpy( pOut, pData, sizeof( T ) );
			return true;
		};

		switch ( uType )
		{
			case HERMES_CLIENT_POINTER_MOTION:
			{
				hermes_client_pointer_motion msg;
				if ( !Read( &msg ) )
					return;

				wlserver_lock();
				wlserver_mousemotion( msg.dx, msg.dy, ++m_uInputSequence );
				wlserver_unlock();
				break;
			}

			case HERMES_CLIENT_POINTER_ABSOLUTE:
			{
				hermes_client_pointer_absolute msg;
				if ( !Read( &msg ) )
					return;

				// From the streamed image to the focused window, as touch does it.
				double flX = std::clamp( msg.x, 0.0, 1.0 ) * g_nOutputWidth;
				double flY = std::clamp( msg.y, 0.0, 1.0 ) * g_nOutputHeight;
				flX = ( flX + focusedWindowOffsetX ) * focusedWindowScaleX;
				flY = ( flY + focusedWindowOffsetY ) * focusedWindowScaleY;

				wlserver_lock();
				wlserver_mousewarp( flX, flY, ++m_uInputSequence, false );
				wlserver_unlock();
				break;
			}

			case HERMES_CLIENT_POINTER_BUTTON:
			{
				hermes_client_pointer_button msg;
				if ( !Read( &msg ) )
					return;

				const bool bPressed = !!msg.pressed;
				// Repeats from the viewer would leave the seat's count off by one.
				if ( bPressed == m_HeldButtons.contains( msg.button ) )
					break;

				if ( bPressed )
					m_HeldButtons.insert( msg.button );
				else
					m_HeldButtons.erase( msg.button );

				wlserver_lock();
				wlserver_mousebutton( int( msg.button ), bPressed, ++m_uInputSequence );
				wlserver_unlock();
				break;
			}

			case HERMES_CLIENT_POINTER_AXIS:
			{
				hermes_client_pointer_axis msg;
				if ( !Read( &msg ) )
					return;

				wlserver_lock();
				wlserver_mousewheel( msg.value120_x / 120.0, msg.value120_y / 120.0, ++m_uInputSequence );
				wlserver_unlock();
				break;
			}

			case HERMES_CLIENT_KEY:
			{
				hermes_client_key msg;
				if ( !Read( &msg ) )
					return;

				const bool bPressed = !!msg.pressed;
				// Key repeat is the game's business; only state changes go through.
				if ( bPressed == m_HeldKeys.contains( msg.keycode ) )
					break;

				if ( bPressed )
					m_HeldKeys.insert( msg.keycode );
				else
					m_HeldKeys.erase( msg.keycode );

				wlserver_lock();
				wlserver_key( msg.keycode, bPressed, ++m_uInputSequence );
				wlserver_unlock();
				break;
			}

			case HERMES_CLIENT_TOUCH:
			{
				hermes_client_touch msg;
				if ( !Read( &msg ) )
					return;

				const double flX = std::clamp( msg.x, 0.0, 1.0 );
				const double flY = std::clamp( msg.y, 0.0, 1.0 );

				wlserver_lock();
				switch ( msg.type )
				{
					case HERMES_TOUCH_DOWN:
						if ( !m_HeldTouches.contains( msg.id ) )
						{
							m_HeldTouches.insert( msg.id );
							wlserver_touchdown( flX, flY, msg.id, ++m_uInputSequence, &m_Connector );
						}
						break;
					case HERMES_TOUCH_MOTION:
						if ( m_HeldTouches.contains( msg.id ) )
							wlserver_touchmotion( flX, flY, msg.id, ++m_uInputSequence, false, &m_Connector );
						break;
					case HERMES_TOUCH_UP:
						if ( m_HeldTouches.erase( msg.id ) )
							wlserver_touchup( msg.id, ++m_uInputSequence );
						break;
					case HERMES_TOUCH_CANCEL:
						for ( int32_t nId : m_HeldTouches )
							wlserver_touchup( nId, ++m_uInputSequence );
						m_HeldTouches.clear();
						break;
					default:
						hermes_log.debugf( "Ignoring unknown touch type %u", msg.type );
						break;
				}
				wlserver_unlock();
				break;
			}

			case HERMES_CLIENT_TEXT:
			{
				hermes_client_text msg;
				if ( !Read( &msg ) )
					return;

				if ( msg.length > HERMES_MAX_TEXT || uSize != sizeof( msg ) + msg.length )
				{
					SendError( ulGeneration, HERMES_ERROR_PROTOCOL, true, "TEXT length does not match its size" );
					DropClient( "sent a malformed TEXT" );
					return;
				}

				if ( !m_pIME || !msg.length )
					break;

				char szText[ HERMES_MAX_TEXT + 1 ] = {};
				memcpy( szText, pData + sizeof( msg ), msg.length );

				wlserver_lock();
				type_text( m_pIME, szText );
				wlserver_unlock();
				break;
			}

			default:
				break;
		}
	}

	void CHermesBackend::ReleaseHeldInput()
	{
		if ( m_HeldKeys.empty() && m_HeldButtons.empty() && m_HeldTouches.empty() )
			return;

		// A viewer that vanishes mid-press must not leave the game with a stuck key.
		wlserver_lock();
		for ( uint32_t uKey : m_HeldKeys )
			wlserver_key( uKey, false, ++m_uInputSequence );
		for ( uint32_t uButton : m_HeldButtons )
			wlserver_mousebutton( int( uButton ), false, ++m_uInputSequence );
		for ( int32_t nId : m_HeldTouches )
			wlserver_touchup( nId, ++m_uInputSequence );
		wlserver_unlock();

		m_HeldKeys.clear();
		m_HeldButtons.clear();
		m_HeldTouches.clear();
	}

	void CHermesBackend::SendError( uint64_t ulGeneration, uint32_t uCode, bool bFatal, const char *pszMessage )
	{
		hermes_log.errorf( "%s", pszMessage );

		hermes_server_error msg = MakeMessage<hermes_server_error>( HERMES_SERVER_ERROR );
		msg.code = uCode;
		msg.fatal = bFatal ? 1 : 0;
		CopyString( msg.message, pszMessage );
		SendToClient( ulGeneration, &msg, sizeof( msg ) );
	}

	bool CHermesBackend::SendToClient( uint64_t ulGeneration, const void *pData, size_t uSize, int nFd )
	{
		std::unique_lock lock{ m_ClientMutex };
		if ( m_nClientFd < 0 || m_ulClientGeneration.load() != ulGeneration )
			return false;

		if ( SendOnSocket( m_nClientFd, pData, uSize, nFd ) )
			return true;

		// A viewer too slow to drain its socket loses frames rather than stalling
		// the compositor. Hang-ups surface on the IO thread's poll.
		if ( errno != EAGAIN && errno != EWOULDBLOCK && errno != EPIPE && errno != ECONNRESET )
			hermes_log.errorf_errno( "sendmsg failed" );
		return false;
	}

	/////////////////////////
	// Backend Instantiator
	/////////////////////////

	template <>
	bool IBackend::Set<CHermesBackend>()
	{
		return Set( new CHermesBackend{} );
	}
}
