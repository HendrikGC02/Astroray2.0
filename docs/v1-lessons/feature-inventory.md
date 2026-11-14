# v1 feature inventory

Every user-visible capability Astroray 1.0 specified, built or planned, as relayed from inventories of
v1's specs, code and design documents. The rewrite's roadmap is compared against this list so that
nothing is silently lost. Status: **built** (landed and working as far as v1's own records show),
**partial** (landed with a stated gap, or on one backend only, or v1's records disagree about whether it
was finished), **planned-not-built**. Where v1 marked something done but left its acceptance checks
unticked, it is listed as partial with "status unclear". Inclusion here is not a commitment for 2.0.

## Materials

- Lambertian diffuse — built
- Oren-Nayar rough diffuse — built
- Phong diffuse plus specular — built
- Perfect mirror — built
- GGX conductor metal with multiple-scattering energy compensation — built
- Smooth dielectric glass with Fresnel and colour tint — built
- Rough glass with single-scatter energy compensation tables — built
- Rough glass as a multiple-scattering microfacet random walk — partial (CPU only)
- Thin architectural glass — built
- Disney principled BSDF (metallic, specular, clearcoat, sheen, subsurface, transmission, anisotropy) with energy compensation — built
- Cycles-style Principled BSDF: base, metallic with edge tint, specular IOR level and tint, transmission, coat with IOR and tint, sheen with roughness and tint, diffuse roughness, emission, alpha, anisotropy with rotation — built
- Principled thin-film iridescence, dielectric and conductor, per wavelength — built
- Glass BSDF thin film — partial (smooth glass shows no film)
- Principled thin wall (thin translucent sheet) — partial (not verified against references)
- Principled dispersion from Abbe number and scale, CPU and GPU — built
- Dispersive glass with Sellmeier presets (BK7, fused silica, SF11 flint, diamond) and manual coefficients — built
- Metallic BSDF node, F82 edge-tint mode — built
- Metallic BSDF node, physical conductor mode from IOR and extinction spectra — planned-not-built (warns and falls back)
- Tabulated complex-IOR metal presets (gold, silver, copper, aluminium) — planned-not-built
- Approximate subsurface scattering inside Principled — built
- Random-walk subsurface (BSSRDF) — partial (CPU prototype)
- Subsurface IOR and subsurface method selector — planned-not-built
- Principled Hair (Chiang model) with melanin, absorption and direct-colour modes, coat, offset, randomness — partial (GPU spectral mode renders wrong)
- Spectral melanin absorption for hair — partial (CPU)
- Legacy Kajiya-Kay Hair BSDF — partial (approximated)
- Two-sided material wrapper — built
- Normal and bump perturbation wrapper over any material — built
- Materials expressed as a shared closure graph (diffuse, conductor, dielectric, clearcoat, sheen, emission, thin film) on CPU and GPU — built
- Texture-driven lobe weights (metallic, roughness, transmission) inside Mix Shader on GPU — partial
- Standalone BSDF nodes (Diffuse and others) honouring linked image textures — planned-not-built
- Authored spectrum replaces RGB reflectance (no RGB round trip) — partial (CPU exact, GPU approximated)
- IR/UV response node attaching a band reflectance — built (non-destructive)
- Coloured glass absorption — planned-not-built
- Filter Glossy (roughness floor along glossy paths) — built (awaiting merge at v1's end)
- Option to let capped glass bounces pass through instead of terminating — partial
- Principled Weight, Tangent and Coat Normal inputs, distribution selector — planned-not-built
- Per-material capability report; unsupported materials raise instead of turning grey — built
- Material zoo contact sheet of every registered material — built

## Textures and nodes

- Image textures with UV, multiple and named UV layers — built
- Image extension modes repeat, extend, clip, mirror — built
- Image interpolation Linear, Cubic, Smart — planned-not-built (Closest only)
- Image alpha handling matched to Blender — partial
- Mip maps, texture cache, UDIM tiles — planned-not-built
- Procedural textures Checker, Gradient, Magic, Wave, Brick, Musgrave — built
- Noise in fBM, multifractal, hybrid, ridged and hetero modes; White Noise — built
- Voronoi with distance metrics, F1, F2, smooth F1, distance to edge, n-sphere — built
- Procedurals on GPU through a 64³ bake — partial (Generated coordinates only; Object coordinates fall back to flat colour)
- Texture Coordinate modes Generated, Object, Camera, Window, Reflection, Normal, UV — built
- Full 3D Mapping node — built
- Vector Math (all operations), Vector Rotate, Clamp, Math and Mix clamp flags — built
- Per-texel colour chains: Color Ramp, Mix, Math, Map Range, Brightness/Contrast, HSV, Invert, Gamma, Separate/Combine Color, RGB to BW — built
- Float Curve and RGB Curves through a typed shader-graph program — built
- Non-affine procedural coordinate warps (Separate XYZ, sine, Combine XYZ) — built (limited operations)
- Texture-driven scalar inputs for roughness, metallic, transmission, IOR — built
- Texture-driven specular, sheen and coat inputs — planned-not-built
- Normal Map node, tangent space with strength — built
- Normal Map object and world space — planned-not-built
- Bump node with strength and distance — built (no ray differentials)
- Displacement node realised as bump — built
- True geometric displacement — planned-not-built
- Geometry Pointiness attribute — built
- Light Path, Attribute, Color Attribute, Object Info inputs — partial
- Math power edge cases matched to Blender — planned-not-built
- Per-render report of approximated, degraded and ignored nodes — built
- Astroray Output node routing to Astroray-specific shading — built
- Spectral Profile and Spectrum Preset nodes (reflectance and emission categories) — built
- Drawn Spectrum node (curve over a wavelength range) — built
- Blackbody Spectrum node with normalisation — built
- Sellmeier Glass node with presets and raw coefficients — built
- NRC cache hint node — built
- Categorised Astroray add menu and node status badges — built
- Spectrum math, Gaussian/bandpass source, spectrum file source — planned-not-built
- Spectral Reflectance, Spectral Filter and Spectral Emission wrapper nodes — planned-not-built
- Element Emission and Spectrum Mix nodes — planned-not-built
- Cloud Volume node — planned-not-built
- Material-only viewport edits without geometry re-export — built

## Lights and emitters

- Point light with true zero radius and optional radius — built
- Spot light with cone and blend — built
- Sun with angular diameter, exact delta at zero — built
- Area light rectangle, disk, ellipse with spread, emitting along -Z — built
- Light energy calibrated in watts to Blender conventions — built (small residual offsets recorded)
- Light power slider in all spectrum modes — built
- Lamps visible in mirror and glossy reflections — built
- Paths continue through a hit lamp — partial (GPU unverified)
- Two-sided MIS for area emitters — built
- Emissive mesh lights — built
- Mesh emitters unified with dedicated lights under one sampling contract — planned-not-built
- Blackbody emission with luminance normalisation — built
- Light spectrum modes: native blackbody or RGB, lamp preset, custom profile — built
- Default blackbody light with colour filter, temperature box and RGB toggle — partial (status unclear in the addon)
- Lamp presets CIE F2, CIE F3, LED 3000 K, 5000 K, 6500 K, sodium vapour, mercury vapour (line strengths from NIST) — built
- Composite spectrum (lamp spectrum behind a colour gel) — built (API only)
- Gel filters on lights in the UI — planned-not-built
- Laser and narrow-line emitter (wavelength, bandwidth) — built (CPU)
- Exact spectral emission on GPU — built
- IES profiles on point and spot lights, CPU and GPU — built
- Light sampler choice: uniform, power, light tree (tree default) — built (variance target not met)
- Goniometric, projection and portal lights, lens-coupled spot — planned-not-built
- Light groups (re-balance emission mechanisms after render) — planned-not-built
- Direct and indirect firefly clamps — built
- Alpha-transparent shadows — built (single hit)

## Sky and world

- Solid world colour and strength, black at strength zero — built
- HDRI environment with rotation, tint and strength — built
- HDRI importance sampling with MIS — built
- Spectral environment atlas built at load — partial (status unclear)
- Sky Texture (Preetham/Perez) baked to an environment — built
- Sky sun disc with sharp shadows — built
- Spectral Nishita sky evaluated in the engine — partial (in progress)
- Sky altitude, ozone and ground albedo — planned-not-built (warned)
- Hosek-Wilkie analytic sky — planned-not-built
- Rayleigh analytic sky for non-visible bands — partial (status unclear)
- Per-wavelength sky with UV and IR — planned-not-built
- Magenta fallback for missing images and HDRIs — built
- World volume fog — built

## Volumes

- Homogeneous world volume, absorption plus Henyey-Greenstein scattering — built
- Constant-density object medium — built
- Heterogeneous grid volumes from Blender Volume and OpenVDB — built
- Delta and ratio tracking, equiangular sampling, volume NEE — built (CPU; GPU stage built)
- Principled Volume: density, colour, absorption colour, anisotropy, emission, blackbody intensity and tint, temperature grid — partial (GPU emission missing)
- Volume Absorption and Volume Scatter nodes — partial
- Mesh-bounded volumes, nested and with the camera inside — partial (CPU)
- Procedural density on mesh volumes — planned-not-built
- Volume direct and indirect passes; volume bounce limit honoured — built
- Volume motion blur from a velocity grid, or dense-media multiple-scatter gain — planned-not-built
- Spectral cloud material (water and ice optics, Mie, ice habits, fogbow, glory, corona) — planned-not-built

## Cameras and sensors

- Perspective camera with sensor fit and lens shift; viewport and render framing match — built
- Depth of field from f-stop and focus object or distance — built
- Aperture blades and rotation — planned-not-built
- Clip start and end — partial (CPU)
- Orthographic viewport alignment — planned-not-built
- Panoramic cameras (fisheye, equirectangular) — planned-not-built
- Camera motion blur with shutter position — built
- Object motion blur — built; deformation motion blur — partial
- Shutter curves and rolling shutter — planned-not-built
- Pixel filters box, Gaussian, Blackman-Harris with Blender's width mapping — built
- Wavelength bands: visible, near IR, UV, custom — built
- Colourmaps grayscale, inferno, viridis, hot, IR false colour — built
- Multi-band false-colour composite — partial (status unclear)
- CIE 1931 2° observer — built
- Selectable observer and measured camera spectral sensitivity — planned-not-built
- Instrument channels from filter transmission times detector QE — planned-not-built
- Telescope PSF per band, detector noise, ADU output — planned-not-built
- Raw relative band plane and calibrated radiance output — planned-not-built
- Object holdout — partial (CPU)

## Passes and outputs

- Albedo, normal and depth guide passes, CPU and GPU — built
- Light-path passes: diffuse, glossy, transmission (direct and indirect), emission, environment, volume — built
- Cryptomatte object and material with manifest — partial
- Camera motion vectors — built
- Sample-count and bounce-count heat maps, convergence tracker — built
- OIDN denoiser on CUDA with guides — built
- OptiX denoiser including temporal mode — built
- Transparent film alpha — built
- Transparent glass over film — planned-not-built
- Multi-layer EXR output — partial
- HDR output with exact solid background — partial
- Spectral data cube (FITS and EXR) with variance, path depth and ray count — planned-not-built
- Light path expressions — planned-not-built
- Degradation report in the UI and logs — built

## Render settings, sampling and caustics

- Spectral path tracer as default, 4 hero wavelengths, luminance-weighted wavelength sampling — built
- Adaptive sampling, CPU and GPU — built
- Progressive low-discrepancy (Sobol) sampler — partial (opt-in)
- Per-type bounce limits and caustic toggles honoured — built
- Native Blender render settings read as the engine's controls — built (several settings shown not to change the render)
- Device auto, CPU, GPU, with an explicit error instead of silent fallback — built
- Photon-map caustics with physical photon power, per-light emission and SPD-sampled photon wavelengths — built (GPU parts partly unverified)
- Specular manifold and specular-polynomial caustics, including the raindrop rainbow chain — partial (CPU)
- Flat-prism rainbow on GPU — partial (noisy)
- Path guiding — partial (CPU)
- ReSTIR direct lighting — built (CPU prototype)
- Neural radiance cache integrator — built (opt-in, slower than path tracing)
- Ambient occlusion integrator — built
- Instancing with a two-level BVH — built (GPU)
- OptiX hardware traversal — partial (triangles only)

## Viewport and tools

- Rendered viewport with progressive refinement — built
- Reduced-resolution navigation — built
- Camera-only updates skip geometry upload; depsgraph-driven incremental sync — built
- Off-thread viewport worker with bounded cancel — partial (opt-in)
- CUDA prewarm on entering rendered mode — built
- Viewport pass selector and optional OIDN — built
- Diagnostics panel (module, features, GPU, fallback reason, integrator capabilities) — built
- Draft, Preview and Final presets — built
- Spectral profile dropdown with reflectance-curve preview — built
- Black hole panel with presets (Sgr A*, M87*, stellar-mass HMXB) — built
- Spectral library picker and View Layer pass panel — planned-not-built
- Showcase and benchmark runner, convergence plots — built
- Standalone command-line renderer — built
- Offline .blend importer — partial
- Bulk mesh upload; curves, surfaces, text and metaballs rendered as meshes — built
- Curves as native hair primitives (ribbon and thick) — built

## Astrophysics

- Schwarzschild and Kerr black holes (spin honoured) — built
- Observer distance controlling shadow size — built
- Thin Novikov-Thorne disk with invariant transfer and momentum redshift — built
- Gravitational and Doppler wavelength shift — built
- Slim disk — partial (status unclear)
- ADAF — partial (visual tuning open)
- Synchrotron jet — built
- Black hole Blender object with disk, ADAF and jet sub-panels — built
- HII region line emission with Case-B ratios — planned-not-built
- Line-emissivity tables over density, temperature, ionisation — planned-not-built
- Dust extinction and reflection nebula — planned-not-built
- Element line-spectrum library (H to Zn and others, LTE, Voigt) — planned-not-built
- HMXB Roche-lobe geometry and light curve — planned-not-built
- Weak lensing post pass; strong lensing — planned-not-built
- Emission-mechanism light groups and photon-ring subimages — planned-not-built

## Data I/O

- Measured spectral library: 40 reflectances and 7 lamps, 300-2500 nm — built
- Register a custom spectral profile from Python — built
- Custom CSV reflectance spectra — partial (status unclear)
- FITS image as texture — built (optional build)
- FITS cube as volume — planned-not-built
- HDF5, yt and npy simulation fields to volumes — planned-not-built
- SPH particles to grid — planned-not-built
- OpenVDB import — built
- OpenEXR output — built
- IES LM-63 files — built
- Demand-loaded textures and host-memory spill for large data — planned-not-built

## Ideas that were raised and never specified

- Diffraction gratings: named by the owner more than once, and in v1 specs only as "deferred until a science case names them"; no design exists.
- Line absorption spectra (owner's words), and molecular spectra (deferred pending a licence decision).
- Fluorescence: listed only as an explicit non-goal.
- Gemstones beyond the diamond Sellmeier preset (a brilliant-diamond showcase was specified, a gem material was not).
- Cloth beyond sheen; measured BSDF loading (stated non-goal).
- The owner's list of custom material types from early planning (not recovered).
- In-air rainbow fan, which needs volumetric photon scattering.
- Showcase tiles: wine-glass caustic, volumetric fog with god rays, Blender viewport side by side.
- Study of a competing spectral Blender add-on and its forum.
- Per-wavelength denoising, temporal and AOV denoising in a newer OIDN.
- Multi-GPU rendering; JPEG XL output.
- ReSTIR GI, BDPT, MLT, SPPM, VCM (VCM ruled out by the owner), neural path guiding.
- Spectral ray differentials and per-bounce spectral MIS (prototype idea only).
- Polarised synchrotron emission, X-ray microphysics, Comptonisation, cluster lensing.
- Telescope seeing, adaptive optics, coronagraphs (stated non-goals).
- Neutron star magnetosphere example scene.
- Indirect-only object visibility; light and shadow linking; CryptoAsset.
- Metameric constrained uplifting of environment emission.

## Added after review (items the first pass missed)

- Analytic sphere primitive with an exact multi-bounce chain, used as a dispersion and caustic oracle (built).
- Gemstone material and brilliant-cut diamond showcase (partial: Sellmeier preset only).
- Input nodes Layer Weight, Fresnel, Backfacing, Ray Length, Hair Info (partial).
- Per-object caustic-caster switch (built; photon pre-pass ignored some material kinds).
- Hair near-field model and per-strand random (planned-not-built).
- RGB-lift conventions: reflectance, unbounded and illuminant upsampling kept distinct; authored reflectance clamped to [0,1] (built).
- Line absorption and molecular spectra (planned-not-built; molecules awaited a data-licence decision).
- Verification tooling: reference image bank with perceptual gates, chi-squared sampler harness, furnace-suite rules, coverage matrix against Blender's node list, live A/B harness against Cycles (built; the Cycles-parity parts are not carried over).
