//! Window surfaces (task A5; the browser's OffscreenCanvas surface is B1's, through
//! `Surface::from_wgpu`).
//!
//! As the retired `gpu_wgpu.c`'s wgpu_surface / surface_configure / wgpu_present (last at
//! render/rust 3882d5576): a surface on the window
//! A surface on the window (`DHDR_WINDOW_WIN32`: a = HWND, b = HINSTANCE; XLIB and WAYLAND on
//! Linux; an ANativeWindow on Android), RGBA8Unorm required; present mode Immediate > Mailbox > Fifo (`DHDR_PRESENT_NO_TEARING`: no Immediate;
//! `DHDR_PRESENT_VSYNC`: Fifo); desired maximum frame latency 1; alpha Opaque; usage
//! RENDER_ATTACHMENT, plus COPY_SRC when the surface offers it (`DHDR_SURFACE_COPY_SRC`, for the
//! test hook). Configured to the window's size, reconfigured when it changes or the frame is
//! Outdated / Lost (once; a second Outdated / Lost fails, as C's wgpu_present); Timeout and
//! Occluded skip the frame. `present_exact` submits the exact work first (the scanout, as C's
//! `submit_all`), then draws the display texture (or CPU / movie pixels written into it first
//! through `Exact::display_upload`, as scanout would) with present.wgsl (`pipe.rs`); no display
//! texture yet skips. The acquired frame stays here until `present`; the export runs the test
//! hook on it first (`frame_texture`). Never blocks on vsync, never suspends.
//!
//! Referee: `test_gs_wgpu.py --impl rust --arch x86_64 --only present` (A5).
use crate::abi::*;
use crate::device::Gpu;
use crate::exact::display::Display;
use crate::exact::Exact;
use crate::present::pipe::PresentPipe;

/// Stand-ins for `exact/display.rs`'s accessors (A4: `Display::view`, `Display::size`,
/// `Exact::display_upload`, same signatures) while render/rust still has the skeleton's
/// `Display`. Inherent methods take precedence over trait methods, so once A4's display.rs is
/// merged the calls below reach A4's methods and these defaults go unused: delete both traits
/// then. Until then an exact frame has no display texture and is skipped (0).
#[allow(dead_code)]
trait SkeletonDisplay {
    fn view(&self) -> Option<&wgpu::TextureView> {
        None
    }
    fn size(&self) -> (u32, u32) {
        (0, 0)
    }
}

impl SkeletonDisplay for Display {}

#[allow(dead_code)]
trait SkeletonExact {
    fn display_upload(&mut self, pixels: &[u32], width: u32, height: u32) -> Result<(), String> {
        let _ = pixels;
        Err(format!("no display texture for a {width} x {height} frame (exact/display.rs not merged)"))
    }
}

impl SkeletonExact for Exact {}

/// The picture texture of a surface used without an exact renderer (pixels only).
struct Own {
    texture: wgpu::Texture,
    view: wgpu::TextureView,
    width: u32,
    height: u32,
}

pub struct Surface {
    pub gpu: &'static Gpu,
    /// The acquired frame, until `present` (dropped before the surface).
    frame: Option<wgpu::SurfaceTexture>,
    surface: wgpu::Surface<'static>,
    usage: wgpu::TextureUsages,
    present_mode: wgpu::PresentMode,
    alpha_mode: wgpu::CompositeAlphaMode,
    /// The configured size; 0 x 0 before the first configure.
    width: u32,
    height: u32,
    pipe: PresentPipe,
    own: Option<Own>,
}

/// `pixels` as bytes (RGBA8, little-endian words).
fn bytes_of(pixels: &[u32]) -> &[u8] {
    // SAFETY: u32 has no padding and every byte pattern is a valid u8; the length is exact.
    unsafe { std::slice::from_raw_parts(pixels.as_ptr() as *const u8, std::mem::size_of_val(pixels)) }
}

/// Writes w x h `pixels` into `texture` (queue write: lands before the next submit).
fn write_pixels(gpu: &Gpu, texture: &wgpu::Texture, pixels: &[u32], w: u32, h: u32) {
    gpu.queue.write_texture(
        wgpu::TexelCopyTextureInfo {
            texture,
            mip_level: 0,
            origin: wgpu::Origin3d::ZERO,
            aspect: wgpu::TextureAspect::All,
        },
        bytes_of(&pixels[..w as usize * h as usize]),
        wgpu::TexelCopyBufferLayout { offset: 0, bytes_per_row: Some(w * 4), rows_per_image: Some(h) },
        wgpu::Extent3d { width: w, height: h, depth_or_array_layers: 1 },
    );
}

impl Surface {
    /// `dhdr_surface_create`: present.wgsl's pipeline, then a surface on the Win32 window.
    pub fn create(gpu: &'static Gpu, window: &DhdrWindow, present_flags: u32) -> Result<Surface, String> {
        let surface = match window.kind {
            DHDR_WINDOW_ANDROID => Self::android(gpu, window)?,
            DHDR_WINDOW_WIN32 => Self::win32(gpu, window)?,
            DHDR_WINDOW_XLIB => Self::xlib(gpu, window)?,
            DHDR_WINDOW_WAYLAND => Self::wayland(gpu, window)?,
            k => return Err(format!("window kind {k} is not supported")),
        };
        let pipe = PresentPipe::create(gpu).ok_or("no present pipeline")?;
        Self::from_wgpu(gpu, surface, present_flags, pipe)
    }

    /// `DHDR_WINDOW_ANDROID`: a = the ANativeWindow (SDL's SDL_PROP_WINDOW_ANDROID_WINDOW_POINTER),
    /// on Vulkan (finding 091). The Vulkan surface holds its own reference to the window.
    #[cfg(target_os = "android")]
    fn android(gpu: &Gpu, window: &DhdrWindow) -> Result<wgpu::Surface<'static>, String> {
        let native = std::ptr::NonNull::new(window.a as usize as *mut std::ffi::c_void).ok_or("NULL ANativeWindow")?;
        let handle = raw_window_handle::AndroidNdkWindowHandle::new(native);
        // SAFETY: C keeps the window alive until dhdr_surface_destroy (present.c drops the surface
        // when the app goes to the background, before SDL releases the window).
        unsafe {
            gpu.instance.create_surface_unsafe(wgpu::SurfaceTargetUnsafe::RawHandle {
                raw_display_handle: Some(raw_window_handle::RawDisplayHandle::Android(
                    raw_window_handle::AndroidDisplayHandle::new(),
                )),
                raw_window_handle: raw_window_handle::RawWindowHandle::AndroidNdk(handle),
            })
        }
        .map_err(|e| format!("create_surface: {e}"))
    }

    #[cfg(not(target_os = "android"))]
    fn android(gpu: &Gpu, window: &DhdrWindow) -> Result<wgpu::Surface<'static>, String> {
        let _ = (gpu, window);
        Err("Android windows are the Android build's only".into())
    }

    #[cfg(not(target_arch = "wasm32"))]
    fn win32(gpu: &Gpu, window: &DhdrWindow) -> Result<wgpu::Surface<'static>, String> {
        let hwnd = std::num::NonZeroIsize::new(window.a as isize).ok_or("NULL HWND")?;
        let mut handle = raw_window_handle::Win32WindowHandle::new(hwnd);
        handle.hinstance = std::num::NonZeroIsize::new(window.b as isize);
        // SAFETY: C keeps the window alive until dhdr_surface_destroy (present.c destroys the
        // surface before the window).
        unsafe {
            gpu.instance.create_surface_unsafe(wgpu::SurfaceTargetUnsafe::RawHandle {
                raw_display_handle: Some(raw_window_handle::RawDisplayHandle::Windows(
                    raw_window_handle::WindowsDisplayHandle::new(),
                )),
                raw_window_handle: raw_window_handle::RawWindowHandle::Win32(handle),
            })
        }
        .map_err(|e| format!("create_surface: {e}"))
    }

    #[cfg(target_arch = "wasm32")]
    fn win32(gpu: &Gpu, window: &DhdrWindow) -> Result<wgpu::Surface<'static>, String> {
        let _ = (gpu, window);
        Err("no window surfaces in the browser (dhdr_web_surface)".into())
    }

    /// `DHDR_WINDOW_XLIB` (Linux, an X11 display): `a` = the Window (an XID Drawable, SDL gives
    /// it as a pointer-sized c_ulong), `b` = the Display* of its connection (SDL's
    /// SDL_PROP_WINDOW_X11_WINDOW / _DISPLAY pointers).
    #[cfg(all(unix, not(target_os = "android")))]
    fn xlib(gpu: &Gpu, window: &DhdrWindow) -> Result<wgpu::Surface<'static>, String> {
        let display = std::ptr::NonNull::new(window.b as usize as *mut std::ffi::c_void).ok_or("NULL X11 Display")?;
        if window.a == 0 {
            return Err("X11 window is 0".into());
        }
        let handle = raw_window_handle::XlibWindowHandle::new(window.a as std::os::raw::c_ulong);
        let display = raw_window_handle::XlibDisplayHandle::new(Some(display), 0);
        // SAFETY: C keeps the window and the display alive until dhdr_surface_destroy (present.c
        // destroys the surface before the window closes).
        unsafe {
            gpu.instance.create_surface_unsafe(wgpu::SurfaceTargetUnsafe::RawHandle {
                raw_display_handle: Some(raw_window_handle::RawDisplayHandle::Xlib(display)),
                raw_window_handle: raw_window_handle::RawWindowHandle::Xlib(handle),
            })
        }
        .map_err(|e| format!("create_surface (X11): {e}"))
    }

    #[cfg(not(all(unix, not(target_os = "android"))))]
    fn xlib(gpu: &Gpu, window: &DhdrWindow) -> Result<wgpu::Surface<'static>, String> {
        let _ = (gpu, window);
        Err("X11 windows are the Linux build's only".into())
    }

    /// `DHDR_WINDOW_WAYLAND` (Linux, a Wayland compositor): `a` = the wl_surface*, `b` = the
    /// wl_display* (SDL's SDL_PROP_WINDOW_WAYLAND_SURFACE / _DISPLAY pointers); the Vulkan
    /// surface holds its own reference to both.
    #[cfg(all(unix, not(target_os = "android")))]
    fn wayland(gpu: &Gpu, window: &DhdrWindow) -> Result<wgpu::Surface<'static>, String> {
        use std::ptr::NonNull;
        let display = NonNull::new(window.b as usize as *mut std::ffi::c_void).ok_or("NULL wl_display")?;
        let surface_ptr = NonNull::new(window.a as usize as *mut std::ffi::c_void).ok_or("NULL wl_surface")?;
        let display = raw_window_handle::WaylandDisplayHandle::new(display);
        let handle = raw_window_handle::WaylandWindowHandle::new(surface_ptr);
        // SAFETY: as xlib's above.
        unsafe {
            gpu.instance.create_surface_unsafe(wgpu::SurfaceTargetUnsafe::RawHandle {
                raw_display_handle: Some(raw_window_handle::RawDisplayHandle::Wayland(display)),
                raw_window_handle: raw_window_handle::RawWindowHandle::Wayland(handle),
            })
        }
        .map_err(|e| format!("create_surface (Wayland): {e}"))
    }

    #[cfg(not(all(unix, not(target_os = "android"))))]
    fn wayland(gpu: &Gpu, window: &DhdrWindow) -> Result<wgpu::Surface<'static>, String> {
        let _ = (gpu, window);
        Err("Wayland windows are the Linux build's only".into())
    }

    /// A surface over `surface` (the window's, or B1's OffscreenCanvas): checks the capabilities and
    /// picks the present mode, alpha mode and usage. Configured on the first acquire.
    pub fn from_wgpu(gpu: &'static Gpu, surface: wgpu::Surface<'static>, present_flags: u32, pipe: PresentPipe)
                     -> Result<Surface, String> {
        let caps = surface.get_capabilities(&gpu.adapter);
        if !caps.formats.contains(&wgpu::TextureFormat::Rgba8Unorm) {
            return Err(format!("the surface has no rgba8unorm (formats {:?})", caps.formats));
        }
        let offers = |m: wgpu::PresentMode| caps.present_modes.contains(&m);
        let present_mode = if present_flags & DHDR_PRESENT_VSYNC != 0 {
            wgpu::PresentMode::Fifo
        } else if present_flags & DHDR_PRESENT_NO_TEARING == 0 && offers(wgpu::PresentMode::Immediate) {
            wgpu::PresentMode::Immediate
        } else if offers(wgpu::PresentMode::Mailbox) {
            wgpu::PresentMode::Mailbox
        } else {
            wgpu::PresentMode::Fifo
        };
        let alpha_mode = if caps.alpha_modes.contains(&wgpu::CompositeAlphaMode::Opaque) || caps.alpha_modes.is_empty() {
            wgpu::CompositeAlphaMode::Opaque
        } else {
            caps.alpha_modes[0]
        };
        // COPY_SRC lets the test hook read the presented texture back; not needed otherwise.
        let usage = wgpu::TextureUsages::RENDER_ATTACHMENT | (caps.usages & wgpu::TextureUsages::COPY_SRC);
        Ok(Surface { gpu, frame: None, surface, usage, present_mode, alpha_mode, width: 0, height: 0, pipe, own: None })
    }

    /// `DHDR_SURFACE_COPY_SRC` when the frames can be copied from.
    pub fn usage(&self) -> u32 {
        if self.usage.contains(wgpu::TextureUsages::COPY_SRC) {
            DHDR_SURFACE_COPY_SRC
        } else {
            0
        }
    }

    /// The configured size (the present pass's target size).
    pub fn size(&self) -> (u32, u32) {
        (self.width, self.height)
    }

    /// DHD_FRAME_LATENCY (1-3, default 1): pictures the swapchain may hold. 1 has the least input
    /// delay, but each acquire then waits until the compositor (G-Sync, overlays) has released the
    /// last picture; 2 or 3 give the render thread room when that release is late (stutter tests).
    fn configure(&mut self, width: u32, height: u32) {
        let max = self.gpu.device.limits().max_texture_dimension_2d;
        let (width, height) = (width.min(max), height.min(max));
        self.surface.configure(&self.gpu.device, &wgpu::SurfaceConfiguration {
            usage: self.usage,
            format: wgpu::TextureFormat::Rgba8Unorm,
            width,
            height,
            present_mode: self.present_mode,
            desired_maximum_frame_latency: frame_latency(),
            alpha_mode: self.alpha_mode,
            view_formats: vec![],
        });
        self.width = width;
        self.height = height;
    }

    /// Configures to window_w x window_h (when that changed, or `reconfigure`), then acquires the
    /// frame: Ok(Some(view)) acquired and kept until `present`; Ok(None) skipped (0 x 0, Timeout,
    /// Occluded); Err failed. The native present pass (native/present.rs) draws through this too.
    pub fn acquire(&mut self, window_w: u32, window_h: u32, reconfigure: bool) -> Result<Option<wgpu::TextureView>, String> {
        self.frame = None; // a frame drawn but never presented is dropped
        if window_w == 0 || window_h == 0 {
            return Ok(None);
        }
        // Android destroys the window's surface on its own thread when the app leaves the screen;
        // a present racing it gets validation errors (configure: "Surface does not support the
        // adapter's queue family"). There they fail this present instead of the process; the C
        // side recreates the surface when the app comes back (gs_gpu_surface_reset).
        #[cfg(target_os = "android")]
        {
            let scope = self.gpu.device.push_error_scope(wgpu::ErrorFilter::Validation);
            let acquired = self.acquire_unscoped(window_w, window_h, reconfigure);
            if let Some(e) = pollster::block_on(scope.pop()) {
                self.frame = None;
                return Err(format!("the surface is gone: {e}"));
            }
            acquired
        }
        #[cfg(not(target_os = "android"))]
        self.acquire_unscoped(window_w, window_h, reconfigure)
    }

    fn acquire_unscoped(&mut self, window_w: u32, window_h: u32, reconfigure: bool) -> Result<Option<wgpu::TextureView>, String> {
        let max = self.gpu.device.limits().max_texture_dimension_2d;
        if reconfigure || (window_w.min(max), window_h.min(max)) != (self.width, self.height) {
            self.configure(window_w, window_h);
        }
        for attempt in 0..2 {
            match self.surface.get_current_texture() {
                wgpu::CurrentSurfaceTexture::Success(t) | wgpu::CurrentSurfaceTexture::Suboptimal(t) => {
                    let view = t.texture.create_view(&wgpu::TextureViewDescriptor::default());
                    self.frame = Some(t);
                    return Ok(Some(view));
                }
                wgpu::CurrentSurfaceTexture::Timeout | wgpu::CurrentSurfaceTexture::Occluded => return Ok(None),
                wgpu::CurrentSurfaceTexture::Outdated | wgpu::CurrentSurfaceTexture::Lost if attempt == 0 => {
                    self.configure(window_w, window_h);
                }
                wgpu::CurrentSurfaceTexture::Outdated => return Err("the surface stayed outdated after reconfiguring".into()),
                wgpu::CurrentSurfaceTexture::Lost => return Err("the surface was lost".into()),
                wgpu::CurrentSurfaceTexture::Validation => return Err("get_current_texture: validation error".into()),
            }
        }
        unreachable!("the second attempt always returns")
    }

    /// The acquired frame's texture (the test hook reads it before `present`), if any.
    pub fn frame_texture(&self) -> Option<&wgpu::Texture> {
        self.frame.as_ref().map(|f| &f.texture)
    }

    /// The surface-owned picture texture at w x h (pixels presented without an exact renderer).
    fn own_texture(&mut self, w: u32, h: u32) -> &Own {
        if self.own.as_ref().is_none_or(|o| (o.width, o.height) != (w, h)) {
            let texture = self.gpu.device.create_texture(&wgpu::TextureDescriptor {
                label: Some("present pixels"),
                size: wgpu::Extent3d { width: w, height: h, depth_or_array_layers: 1 },
                mip_level_count: 1,
                sample_count: 1,
                dimension: wgpu::TextureDimension::D2,
                format: wgpu::TextureFormat::Rgba8Unorm,
                usage: wgpu::TextureUsages::TEXTURE_BINDING | wgpu::TextureUsages::COPY_DST,
                view_formats: &[],
            });
            let view = texture.create_view(&wgpu::TextureViewDescriptor::default());
            self.own = Some(Own { texture, view, width: w, height: h });
        }
        self.own.as_ref().expect("own texture")
    }

    /// `dhdr_present_exact`: 1 acquired and drawn, 0 skipped, -1 failed (logged). As gpu_wgpu.c's
    /// wgpu_present: pending exact work (the scanout) is submitted first; `pixels` (w x h, checked
    /// by `ffi.rs`) go into the exact display texture, as scanout would, so a later capture returns
    /// them, even when the window is minimised; without an exact renderer they go into a texture
    /// of the surface's own. No display texture yet (no scanout, no pixels) skips.
    pub fn present_exact(&mut self, exact: Option<&mut Exact>, pixels: Option<&[u32]>, w: u32, h: u32,
                         window_w: u32, window_h: u32) -> i32 {
        let gpu = self.gpu;
        let log = |e: &str| crate::host::log(&format!("dhd_render: present: {e}"));
        // The picture: (view, width, height).
        let source = match exact {
            Some(e) => {
                e.submit(); // the scanout, before the picture is replaced or shown
                if let Some(pixels) = pixels {
                    // C returns 0 (skip, keep presenting) for a frame the display texture can't hold.
                    if let Err(err) = e.display_upload(pixels, w, h) {
                        log(&err);
                        return 0;
                    }
                }
                let Some(view) = e.display.view() else { return 0 };
                let (dw, dh) = e.display.size();
                (view.clone(), dw, dh)
            }
            None => match pixels {
                Some(pixels) => {
                    let own = self.own_texture(w, h);
                    write_pixels(gpu, &own.texture, pixels, w, h);
                    (own.view.clone(), w, h)
                }
                None => return 0, // nothing to show
            },
        };
        let target = match self.acquire(window_w, window_h, false) {
            Ok(Some(view)) => view,
            Ok(None) => return 0,
            Err(e) => {
                log(&e);
                return -1;
            }
        };
        let mut encoder = gpu.device.create_command_encoder(&wgpu::CommandEncoderDescriptor { label: Some("present") });
        self.pipe.draw(gpu, &mut encoder, &source.0, &target, [source.1, source.2, self.width, self.height]);
        gpu.queue.submit([encoder.finish()]);
        1
    }

    /// `dhdr_surface_present`: presents the frame the last call that returned 1 drew.
    pub fn present(&mut self) {
        if let Some(frame) = self.frame.take() {
            frame.present();
        }
    }
}

/// DHD_FRAME_LATENCY, read once (Surface::configure).
fn frame_latency() -> u32 {
    static LATENCY: std::sync::OnceLock<u32> = std::sync::OnceLock::new();
    *LATENCY.get_or_init(|| {
        std::env::var("DHD_FRAME_LATENCY").ok().and_then(|s| s.trim().parse::<u32>().ok()).map_or(1, |n| n.clamp(1, 3))
    })
}
