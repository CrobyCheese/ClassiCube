#include "Core.h"
#if defined CC_BUILD_3DS
#include "_GraphicsBase.h"
#include "Errors.h"
#include "Logger.h"
#include "Window.h"
#include <3ds.h>
#define BUFFER_BASE_PADDR OS_VRAM_PADDR // VRAM physical address
#include "../third_party/citro3d.c"

// autogenerated from the .v.pica files by Makefile
extern const u8  coloured_shbin[];
extern const u32 coloured_shbin_size;

extern const u8  textured_shbin[];
extern const u32 textured_shbin_size;

extern const u8  offset_shbin[];
extern const u32 offset_shbin_size;

#define DISPLAY_TRANSFER_FLAGS \
	(GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) | \
	GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) | \
	GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))
	
static void GPUBuffers_DeleteUnreferenced(void);
static void GPUTextures_DeleteUnreferenced(void);
static cc_uint32 frameCounter1;
static PackedCol clear_color;
static cc_bool rendering3D;
	
	
/*########################################################################################################################*
*------------------------------------------------------Vertex shaders-----------------------------------------------------*
*#########################################################################################################################*/
#define UNI_MVP_MATRIX  (1 << 0)
#define UNI_TEX_OFFSETS (1 << 1)
// cached uniforms (cached for multiple programs)
static C3D_Mtx _mvp;
static float texOffsetX, texOffsetY;
static int texOffset;

struct CCShader {
	DVLB_s* dvlb;
	shaderProgram_s program;
	int uniforms;     // which associated uniforms need to be resent to GPU
	int locations[2]; // location of uniforms (not constant)
};
static struct CCShader* gfx_activeShader;
static struct CCShader shaders[3];

static void Shader_Alloc(struct CCShader* shader, const u8* binData, int binSize) {
	shader->dvlb = DVLB_ParseFile((u32*)binData, binSize);
	shaderProgramInit(&shader->program);
	shaderProgramSetVsh(&shader->program, &shader->dvlb->DVLE[0]);
	
	shader->locations[0] = shaderInstanceGetUniformLocation(shader->program.vertexShader, "MVP");
	shader->locations[1] = shaderInstanceGetUniformLocation(shader->program.vertexShader, "tex_offset");
}

static void Shader_Free(struct CCShader* shader) {
	shaderProgramFree(&shader->program);
	DVLB_Free(shader->dvlb);
}

// Marks a uniform as changed on all programs
static void DirtyUniform(int uniform) {
	for (int i = 0; i < Array_Elems(shaders); i++) 
	{
		shaders[i].uniforms |= uniform;
	}
}

// Sends changed uniforms to the GPU for current program
static void ReloadUniforms(void) {
	struct CCShader* s = gfx_activeShader;
	if (!s) return; // NULL if context is lost

	if (s->uniforms & UNI_MVP_MATRIX) {
		C3D_FVUnifMtx4x4(s->locations[0], &_mvp);
		s->uniforms &= ~UNI_MVP_MATRIX;
	}
	
	if (s->uniforms & UNI_TEX_OFFSETS) {
		C3D_FVUnifSet(s->locations[1],
				texOffsetX, texOffsetY, 0.0f, 0.0f);
		s->uniforms &= ~UNI_TEX_OFFSETS;
	}
}

// Switches program to one that can render current vertex format and state
// Loads program and reloads uniforms if needed
static void SwitchProgram(void) {
	struct CCShader* shader;
	int index = 0;

	if (gfx_format == VERTEX_FORMAT_TEXTURED) index++;
	if (texOffset) index = 2;

	shader = &shaders[index];
	if (shader != gfx_activeShader) {
		gfx_activeShader = shader;
		C3D_BindProgram(&shader->program);
	}
	ReloadUniforms();
}


/*########################################################################################################################*
*---------------------------------------------------------General---------------------------------------------------------*
*#########################################################################################################################*/
static C3D_RenderTarget* topTargetLeft;
static C3D_RenderTarget* topTargetRight;
static C3D_RenderTarget* bottomTarget;

static void AllocShaders(void) {
	Shader_Alloc(&shaders[0], coloured_shbin, coloured_shbin_size);
	Shader_Alloc(&shaders[1], textured_shbin, textured_shbin_size);
	Shader_Alloc(&shaders[2], offset_shbin,   offset_shbin_size);
}

static void FreeShaders(void) {
	for (int i = 0; i < Array_Elems(shaders); i++) 
	{
		Shader_Free(&shaders[i]);
	}
}

static void SetDefaultState(void) {
	Gfx_SetFaceCulling(false);
	Gfx_SetAlphaTest(false);
	Gfx_SetDepthWrite(true);
}

static void InitCitro3D(void) {	
	C3D_Init(C3D_DEFAULT_CMDBUF_SIZE * 4);

	topTargetLeft = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH24);
	C3D_RenderTargetSetOutput(topTargetLeft, GFX_TOP, GFX_LEFT, DISPLAY_TRANSFER_FLAGS);

	// Even though the bottom screen is 320 pixels wide, we use 400 here so that the same ortho matrix
	// can be used for both screens. The output is clipped to the actual screen width, anyway.
	bottomTarget = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH24);
	C3D_RenderTargetSetOutput(bottomTarget, GFX_BOTTOM, GFX_LEFT, DISPLAY_TRANSFER_FLAGS);

	gfxSetDoubleBuffering(GFX_TOP, true);
	SetDefaultState();
	AllocShaders();
}

static GfxResourceID white_square;
void Gfx_Create(void) {
	if (!Gfx.Created) InitCitro3D();
	
	Gfx.MaxTexWidth  = 1024;
	Gfx.MaxTexHeight = 1024;
	Gfx.MaxTexSize   = 512 * 512;
	
	Gfx.MinTexWidth  = 8;
	Gfx.MinTexHeight = 8;
	Gfx.Created      = true;
	gfx_vsync        = true;
	
	Gfx_RestoreState();
}

void Gfx_Free(void) {
	Gfx_FreeState();
	// FreeShaders()
	// C3D_Fini()
}

cc_bool Gfx_TryRestoreContext(void) { return true; }

void Gfx_RestoreState(void) {
	InitDefaultResources();
 	
	// 8x8 dummy white texture
	//  (textures must be at least 8x8, see C3D_TexInitWithParams source)
	struct Bitmap bmp;
	BitmapCol pixels[8*8];
	Mem_Set(pixels, 0xFF, sizeof(pixels));
	Bitmap_Init(bmp, 8, 8, pixels);
	white_square = Gfx_CreateTexture(&bmp, 0, false);
}

void Gfx_FreeState(void) {
	FreeDefaultResources(); 
	Gfx_DeleteTexture(&white_square);
}

void Gfx_3DS_SetRenderScreen(enum Screen3DS screen) {
	C3D_FrameDrawOn(screen == TOP_SCREEN ? topTargetLeft : bottomTarget);
}


/*########################################################################################################################*
*----------------------------------------------------Stereoscopic support-------------------------------------------------*
*#########################################################################################################################*/
static void Calc3DProjection(int dir, struct Matrix* proj) {
	struct Matrix proj_adj = *proj;

	// See mtx_perspstereotilt
	float slider = osGet3DSliderState();
	float iod    = (slider / 3) * dir;
	float shift  = iod / (2.0f * 2.0f);

	proj_adj.row3.y = 1.0f * shift * -proj->row1.y;
	Gfx.Projection = proj_adj;
}

void Gfx_Set3DLeft(struct Matrix* proj, struct Matrix* view) {
	Calc3DProjection(-1, proj);
	rendering3D = true;
}

void Gfx_Set3DRight(struct Matrix* proj, struct Matrix* view) {
	Calc3DProjection(+1, proj);

	if (!topTargetRight) {
		topTargetRight = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
		C3D_RenderTargetSetOutput(topTargetRight, GFX_TOP, GFX_RIGHT, DISPLAY_TRANSFER_FLAGS);
	}

	C3D_RenderTargetClear(topTargetRight, C3D_CLEAR_ALL, clear_color, 0);
	C3D_FrameDrawOn(topTargetRight);
}

void Gfx_End3D(struct Matrix* proj, struct Matrix* view) {
	Gfx.Projection = *proj;
}


/*########################################################################################################################*
*--------------------------------------------------------GPU Textures-----------------------------------------------------*
*#########################################################################################################################*/
struct GPUTexture;
struct GPUTexture {
	cc_uint32* data;
	C3D_Tex texture;
	struct GPUTexture* next;
	cc_uint32 lastFrame;
};
static struct GPUTexture* del_textures_head;
static struct GPUTexture* del_textures_tail;

struct GPUTexture* GPUTexture_Alloc(void) {
	struct GPUTexture* tex = Mem_AllocCleared(1, sizeof(struct GPUTexture), "GPU texture");
	return tex;
}

// can't delete textures until not used in any frames
static void GPUTexture_Unref(GfxResourceID* resource) {
	struct GPUTexture* tex = (struct GPUTexture*)(*resource);
	if (!tex) return;
	*resource = NULL;
	
	LinkedList_Append(tex, del_textures_head, del_textures_tail);
}

static void GPUTexture_Free(struct GPUTexture* tex) {
	C3D_TexDelete(&tex->texture);
	Mem_Free(tex);
}

static void GPUTextures_DeleteUnreferenced(void) {
	if (!del_textures_head) return;
	
	struct GPUTexture* tex;
	struct GPUTexture* next;
	struct GPUTexture* prev = NULL;
	
	for (tex = del_textures_head; tex != NULL; tex = next)
	{
		next = tex->next;
		
		if (tex->lastFrame + 4 > frameCounter1) {
			// texture was used within last 4 fames
			prev = tex;
		} else {
			// advance the head of the linked list
			if (del_textures_head == tex) 
				del_textures_head = next;
			// update end of linked list if necessary
			if (del_textures_tail == tex)
				del_textures_tail = prev;
			
			// unlink this texture from the linked list
			if (prev) prev->next = next;
			
			GPUTexture_Free(tex);
		}
	}
}


/*########################################################################################################################*
*---------------------------------------------------------Textures--------------------------------------------------------*
*#########################################################################################################################*/
static bool CreateNativeTexture(C3D_Tex* tex, u32 width, u32 height) {
	u32 size = width * height * 4;
	//tex->data = p.onVram ? vramAlloc(total_size) : linearAlloc(total_size);
	tex->data = linearAlloc(size);
	if (!tex->data) return false;

	tex->width  = width;
	tex->height = height;
	tex->param  = GPU_TEXTURE_MODE(GPU_TEX_2D) |
					GPU_TEXTURE_MAG_FILTER(GPU_NEAREST) | GPU_TEXTURE_MIN_FILTER(GPU_NEAREST) |
					GPU_TEXTURE_WRAP_S(GPU_REPEAT)      | GPU_TEXTURE_WRAP_T(GPU_REPEAT);
	tex->fmt    = GPU_RGBA8;
	tex->size   = size;

	tex->border   = 0;
	tex->lodBias  = 0;
	tex->maxLevel = 0;
	tex->minLevel = 0;
	return true;
}

static void TryTransferToVRAM(C3D_Tex* tex) {
	return;
	// NOTE: the linearFree below results in broken texture. maybe no DMA?
	void* vram = vramAlloc(tex->size);
	if (!vram) return;

	C3D_SyncTextureCopy((u32*)tex->data, 0, (u32*)vram, 0, tex->size, 8);
	linearFree(tex->data);
	tex->data = vram;
}

/*static inline cc_uint32 CalcZOrder(cc_uint32 x, cc_uint32 y) {
	// Simplified "Interleave bits by Binary Magic Numbers" from
	// http://graphics.stanford.edu/~seander/bithacks.html#InterleaveTableObvious
	// TODO: Simplify to array lookup?
    	x = (x | (x << 2)) & 0x33;
    	x = (x | (x << 1)) & 0x55;

    	y = (y | (y << 2)) & 0x33;
    	y = (y | (y << 1)) & 0x55;

    return x | (y << 1);
}*/
static inline cc_uint32 CalcZOrder(cc_uint32 a) {
	// Simplified "Interleave bits by Binary Magic Numbers" from
	// http://graphics.stanford.edu/~seander/bithacks.html#InterleaveBMN
	// TODO: Simplify to array lookup?
    	a = (a | (a << 2)) & 0x33;
    	a = (a | (a << 1)) & 0x55;
    	return a;
    	// equivalent to return (a & 1) | ((a & 2) << 1) | (a & 4) << 2;
    	//  but compiles to less instructions
}

// Pixels are arranged in a recursive Z-order curve / Morton offset
// They are arranged into 8x8 tiles, where each 8x8 tile is composed of
//  four 4x4 subtiles, which are in turn composed of four 2x2 subtiles
static void ToMortonTexture(C3D_Tex* tex, int originX, int originY, 
				struct Bitmap* bmp, int rowWidth) {
	unsigned int pixel, mortonX, mortonY;
	unsigned int dstX, dstY, tileX, tileY;
	
	int width = bmp->width, height = bmp->height;
	cc_uint32* dst = tex->data;
	cc_uint32* src = bmp->scan0;

	for (int y = 0; y < height; y++)
	{
		dstY    = tex->height - 1 - (y + originY);
		tileY   = dstY & ~0x07;
		mortonY = CalcZOrder(dstY & 0x07) << 1;

		for (int x = 0; x < width; x++)
		{
			dstX    = x + originX;
			tileX   = dstX & ~0x07;
			mortonX = CalcZOrder(dstX & 0x07);
			pixel   = src[x + (y * rowWidth)];

			dst[(mortonX | mortonY) + (tileX * 8) + (tileY * tex->width)] = pixel;
		}
	}
	// TODO flush data cache GSPGPU_FlushDataCache
}


static GfxResourceID Gfx_AllocTexture(struct Bitmap* bmp, cc_uint8 flags, cc_bool mipmaps) {
	struct GPUTexture* tex = GPUTexture_Alloc();
	bool success = CreateNativeTexture(&tex->texture, bmp->width, bmp->height);
	if (!success) return NULL;
	
	ToMortonTexture(&tex->texture, 0, 0, bmp, bmp->width);
	if (!(flags & TEXTURE_FLAG_DYNAMIC)) TryTransferToVRAM(&tex->texture);
    return tex;
}

void Gfx_UpdateTexture(GfxResourceID texId, int x, int y, struct Bitmap* part, int rowWidth, cc_bool mipmaps) {
 	struct GPUTexture* tex = (struct GPUTexture*)texId;
	ToMortonTexture(&tex->texture, x, y, part, rowWidth);
}

void Gfx_UpdateTexturePart(GfxResourceID texId, int x, int y, struct Bitmap* part, cc_bool mipmaps) {
	Gfx_UpdateTexture(texId, x, y, part, part->width, mipmaps);
}
void Gfx_DeleteTexture(GfxResourceID* texId) {
	GPUTexture_Unref(texId);
}

void Gfx_EnableMipmaps(void) { }
void Gfx_DisableMipmaps(void) { }

void Gfx_BindTexture(GfxResourceID texId) {
	if (!texId) texId = white_square; 
 	struct GPUTexture* tex = (struct GPUTexture*)texId;
	
	tex->lastFrame = frameCounter1;
	C3D_TexBind(0, &tex->texture);
}


/*########################################################################################################################*
*-----------------------------------------------------State management----------------------------------------------------*
*#########################################################################################################################*/
void Gfx_SetFaceCulling(cc_bool enabled) { 
	C3D_CullFace(enabled ? GPU_CULL_BACK_CCW : GPU_CULL_NONE);
}

void Gfx_SetAlphaArgBlend(cc_bool enabled) { }

void Gfx_SetAlphaBlending(cc_bool enabled) { 
	if (enabled) {
		C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA);
	} else {
		C3D_ColorLogicOp(GPU_LOGICOP_COPY);
	}
}

void Gfx_SetAlphaTest(cc_bool enabled) { 
	C3D_AlphaTest(enabled, GPU_GREATER, 0x7F);
}

void Gfx_DepthOnlyRendering(cc_bool depthOnly) {
	cc_bool enabled = !depthOnly;
	SetColorWrite(enabled & gfx_colorMask[0], enabled & gfx_colorMask[1], 
				  enabled & gfx_colorMask[2], enabled & gfx_colorMask[3]);
}

void Gfx_ClearColor(PackedCol color) {
	// TODO find better way?
	clear_color = (PackedCol_R(color) << 24) | (PackedCol_G(color) << 16) | (PackedCol_B(color) << 8) | 0xFF;
}

static cc_bool depthTest, depthWrite;
static int colorWriteMask = GPU_WRITE_COLOR;

static void UpdateWriteState(void) {
	//C3D_EarlyDepthTest(true, GPU_EARLYDEPTH_GREATER, 0);
	//C3D_EarlyDepthTest(false, GPU_EARLYDEPTH_GREATER, 0);
	int writeMask = colorWriteMask;
	if (depthWrite) writeMask |= GPU_WRITE_DEPTH;
	C3D_DepthTest(depthTest, GPU_GEQUAL, writeMask);
}

void Gfx_SetDepthWrite(cc_bool enabled) {
	depthWrite = enabled;
	UpdateWriteState();
}

void Gfx_SetDepthTest(cc_bool enabled) { 
	depthTest = enabled;
	UpdateWriteState();
}

static void SetColorWrite(cc_bool r, cc_bool g, cc_bool b, cc_bool a) {
	int mask = 0;
	if (r) mask |= GPU_WRITE_RED;
	if (g) mask |= GPU_WRITE_GREEN;
	if (b) mask |= GPU_WRITE_BLUE;
	if (a) mask |= GPU_WRITE_ALPHA;
	
	colorWriteMask = mask;
	UpdateWriteState();
}


/*########################################################################################################################*
*-----------------------------------------------------------Misc----------------------------------------------------------*
*#########################################################################################################################*/
static BitmapCol* _3DS_GetRow(struct Bitmap* bmp, int y, void* ctx) {
	u8* fb    = (u8*)ctx;
	// Framebuffer is rotated 90 degrees	
	int width = bmp->width, height = bmp->height;

	for (int x = 0; x < width; x++)
	{
		int addr = (height - 1 - y + x * height) * 3; // TODO -1 or not
		int b = fb[addr + 0];
		int g = fb[addr + 1];
		int r = fb[addr + 2];
		bmp->scan0[x] = BitmapColor_RGB(r, g, b);
	}
	return bmp->scan0;
}

cc_result Gfx_TakeScreenshot(struct Stream* output) {
	BitmapCol tmp[512];
	u16 width, height;
	u8* fb = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, &width, &height);

	// Framebuffer is rotated 90 degrees
	struct Bitmap bmp;
	bmp.scan0  = tmp;
	bmp.width  = height; 
	bmp.height = width;

	return Png_Encode(&bmp, output, _3DS_GetRow, false, fb);
}

void Gfx_GetApiInfo(cc_string* info) {
	String_Format1(info, "-- Using 3DS --\n", NULL);
	PrintMaxTextureInfo(info);
}

void Gfx_SetFpsLimit(cc_bool vsync, float minFrameMs) {
	gfx_minFrameMs = minFrameMs;
	gfx_vsync      = vsync;
}

void Gfx_BeginFrame(void) {
	rendering3D = false;
	// wait for vblank for both screens TODO move to end?
	if (gfx_vsync) C3D_FrameSync();

	C3D_FrameBegin(0);
	C3D_FrameDrawOn(topTargetLeft);

	extern void C3Di_UpdateContext(void);
	C3Di_UpdateContext();
}

void Gfx_ClearBuffers(GfxBuffers buffers) {
	int targets = 0;
	if (buffers & GFX_BUFFER_COLOR) targets |= C3D_CLEAR_COLOR;
	if (buffers & GFX_BUFFER_DEPTH) targets |= C3D_CLEAR_DEPTH;
	
	C3D_RenderTargetClear(topTargetLeft, targets, clear_color, 0);
	C3D_RenderTargetClear(bottomTarget,  targets,           0, 0);
}

void Gfx_EndFrame(void) {
	gfxSet3D(rendering3D);
	C3D_FrameEnd(0);
	//gfxFlushBuffers();
	//gfxSwapBuffers();

	if (gfx_minFrameMs) LimitFPS();
		
	GPUBuffers_DeleteUnreferenced();
	GPUTextures_DeleteUnreferenced();
	frameCounter1++;
}

void Gfx_OnWindowResize(void) { }


/*########################################################################################################################*
*----------------------------------------------------------Buffers--------------------------------------------------------*
*#########################################################################################################################*/
struct GPUBuffer {
	cc_uint32 lastFrame;
	struct GPUBuffer* next;
	int pad1, pad2;
	cc_uint8 data[]; // aligned to 16 bytes
};
static struct GPUBuffer* del_buffers_head;
static struct GPUBuffer* del_buffers_tail;

static struct GPUBuffer* GPUBuffer_Alloc(int count, int elemSize) {
	void* ptr = linearAlloc(16 + count * elemSize);
	return (struct GPUBuffer*)ptr;
}

// can't delete buffers until not used in any frames
static void GPUBuffer_Unref(GfxResourceID* resource) {
	struct GPUBuffer* buf = (struct GPUBuffer*)(*resource);
	if (!buf) return;
	*resource = NULL;
	
	LinkedList_Append(buf, del_buffers_head, del_buffers_tail);
}

static void GPUBuffer_Free(struct GPUBuffer* buf) {
	linearFree(buf);
}

static void GPUBuffers_DeleteUnreferenced(void) {
	if (!del_buffers_head) return;
	
	struct GPUBuffer* buf;
	struct GPUBuffer* next;
	struct GPUBuffer* prev = NULL;
	
	for (buf = del_buffers_head; buf != NULL; buf = next)
	{
		next = buf->next;
		
		if (buf->lastFrame + 4 > frameCounter1) {
			// texture was used within last 4 fames
			prev = buf;
		} else {
			// advance the head of the linked list
			if (del_buffers_head == buf) 
				del_buffers_head = next;
			// update end of linked list if necessary
			if (del_buffers_tail == buf)
				del_buffers_tail = prev;
			
			// unlink this texture from the linked list
			if (prev) prev->next = next;
			
			GPUBuffer_Free(buf);
		}
	}
}


/*########################################################################################################################*
*-------------------------------------------------------Index buffers-----------------------------------------------------*
*#########################################################################################################################*/
static cc_uint16* gfx_indices;

GfxResourceID Gfx_CreateIb2(int count, Gfx_FillIBFunc fillFunc, void* obj) {
	if (!gfx_indices) {
		gfx_indices = linearAlloc(count * sizeof(cc_uint16));
		if (!gfx_indices) Logger_Abort("Failed to allocate memory for index buffer");
	}

	fillFunc(gfx_indices, count, obj);
	return gfx_indices;
}

void Gfx_BindIb(GfxResourceID ib) {
	u32 pa = osConvertVirtToPhys(ib);
	GPUCMD_AddWrite(GPUREG_INDEXBUFFER_CONFIG, (pa - BUFFER_BASE_PADDR) | (C3D_UNSIGNED_SHORT << 31));
}

void Gfx_DeleteIb(GfxResourceID* ib) { }


/*########################################################################################################################*
*-------------------------------------------------------Vertex buffers----------------------------------------------------*
*#########################################################################################################################*/
static cc_uint8* gfx_vertices;

static GfxResourceID Gfx_AllocStaticVb(VertexFormat fmt, int count) {
	return GPUBuffer_Alloc(count, strideSizes[fmt]);
}

void Gfx_BindVb(GfxResourceID vb) {
	struct GPUBuffer* buffer = (struct GPUBuffer*)vb;
	buffer->lastFrame = frameCounter1;
	gfx_vertices = buffer->data;
}

void Gfx_DeleteVb(GfxResourceID* vb) { GPUBuffer_Unref(vb); }

void* Gfx_LockVb(GfxResourceID vb, VertexFormat fmt, int count) {
	struct GPUBuffer* buffer = (struct GPUBuffer*)vb;
	return buffer->data;
}

void Gfx_UnlockVb(GfxResourceID vb) {
	struct GPUBuffer* buffer = (struct GPUBuffer*)vb;
	gfx_vertices = buffer->data;
}


static GfxResourceID Gfx_AllocDynamicVb(VertexFormat fmt, int maxVertices) {
	return GPUBuffer_Alloc(maxVertices, strideSizes[fmt]);
}

void Gfx_BindDynamicVb(GfxResourceID vb) { Gfx_BindVb(vb); }

void* Gfx_LockDynamicVb(GfxResourceID vb, VertexFormat fmt, int count) { 
	struct GPUBuffer* buffer = (struct GPUBuffer*)vb;
	return buffer->data;
}

void Gfx_UnlockDynamicVb(GfxResourceID vb) {
	struct GPUBuffer* buffer = (struct GPUBuffer*)vb;
	gfx_vertices = buffer->data;
}

void Gfx_DeleteDynamicVb(GfxResourceID* vb) { Gfx_DeleteVb(vb); }


/*########################################################################################################################*
*-----------------------------------------------------State management----------------------------------------------------*
*#########################################################################################################################*/
static u32 fogColor;
static C3D_FogLut fog_lut;
static int fogMode = FOG_LINEAR;
static float fogDensity = 1.0f;
static float fogEnd = 32.0f;

void Gfx_SetFog(cc_bool enabled) {
	//C3D_FogGasMode(enabled ? GPU_FOG : GPU_NO_FOG, GPU_DEPTH_DENSITY, false);
	// TODO doesn't work quite right
}

void Gfx_SetFogCol(PackedCol color) {
	// TODO find better way?
	u32 c = (PackedCol_R(color) << 24) | (PackedCol_G(color) << 16) | (PackedCol_B(color) << 8) | 0xFF;
	if (c == fogColor) return;

	fogColor = c;
	C3D_FogColor(c);
}

static void ApplyFog(float* values) {
	float data[256];

	for (int i = 0; i <= 128; i ++)
	{
		float val = values[i];
		if (i < 128) data[i]       = val;
		if (i > 0)   data[i + 127] = val-data[i-1];
	}

	FogLut_FromArray(&fog_lut, data);
	C3D_FogLutBind(&fog_lut);
}

static void UpdateFog(void) {
	float near = 0.01f;
	float far  = Game_ViewDistance;
	float values[129];

	for (int i = 0; i <= 128; i ++)
	{
		float c = FogLut_CalcZ(i / 128.0f, near, far);

		if (fogMode == FOG_LINEAR) {
			values[i] = (fogEnd - c) / fogEnd;
		} else if (fogMode == FOG_EXP) {
			values[i] = expf(-fogDensity * c);
		} else {
			values[i] = expf(-fogDensity * c * c);
		}
	}
	ApplyFog(values);
}

void Gfx_SetFogDensity(float value) {
	if (fogDensity == value) return;

	fogDensity = value;
	UpdateFog();
}

void Gfx_SetFogEnd(float value) {
	if (fogEnd == value) return;

	fogEnd = value;
	UpdateFog();
}

void Gfx_SetFogMode(FogFunc func) {
	fogMode = func;
	UpdateFog();
}


/*########################################################################################################################*
*---------------------------------------------------------Matrices--------------------------------------------------------*
*#########################################################################################################################*/
static C3D_Mtx _view, _proj;

void Gfx_CalcOrthoMatrix(struct Matrix* matrix, float width, float height, float zNear, float zFar) {
	// See Mtx_OrthoTilt in Citro3D for the original basis
	// (it's mostly just a standard orthograph matrix rotated by 90 degrees)
	// Note: The rows/columns are "flipped" over the diagonal axis compared to original basis
	Mem_Set(matrix, 0, sizeof(struct Matrix));
	
	matrix->row2.x = -2.0f / height;
	matrix->row4.x =  1.0f;
	matrix->row1.y = -2.0f / width;
	matrix->row4.y =  1.0f;
	
	matrix->row3.z = 1.0f / (zNear - zFar);		
	matrix->row4.z = 0.5f * (zNear + zFar) / (zNear - zFar) - 0.5f;
	matrix->row4.w = 1.0f;
}

void Gfx_CalcPerspectiveMatrix(struct Matrix* matrix, float fov, float aspect, float zFar) {
	// See Mtx_PerspTilt in Citro3D for the original basis
	// (it's mostly just a standard perspective matrix rotated by 90 degrees) 
	// Note: The rows/columns are "flipped" over the diagonal axis compared to original basis
	float zNear = 0.1f;
	fov = tanf(fov / 2.0f);	 
	Mem_Set(matrix, 0, sizeof(struct Matrix));

	matrix->row2.x =  1.0f / fov;
	matrix->row1.y = -1.0f / (fov * aspect);
	matrix->row4.z = zFar * zNear  / (zNear - zFar);
	matrix->row3.w = -1.0f;
	matrix->row3.z =  1.0f * zNear / (zNear - zFar);
}

void Gfx_LoadMatrix(MatrixType type, const struct Matrix* matrix) {
	C3D_Mtx* dst = type == MATRIX_VIEW ? &_view : &_proj;
	float* src   = (float*)matrix;
	
	// Transpose
	for (int i = 0; i < 4; i++)
	{
		dst->r[i].x = src[0  + i];
		dst->r[i].y = src[4  + i];
		dst->r[i].z = src[8  + i];
		dst->r[i].w = src[12 + i];
	}

	Mtx_Multiply(&_mvp, &_proj, &_view);
	DirtyUniform(UNI_MVP_MATRIX);
	ReloadUniforms();
}


/*void Gfx_LoadMatrix(MatrixType type, const struct Matrix* matrix) {
	if (type == MATRIX_VIEW) _view = *matrix;
	
	// Provided projection matrix assumes landscape display, but 3DS framebuffer
	//  is a rotated portrait display, so need to swap pixel X/Y values to correct that
	// 
	// This can be done by rotating the projection matrix 90 degrees around Z axis
	// https://open.gl/transformations
	if (type == MATRIX_PROJECTION) {
		struct Matrix rot = Matrix_Identity;
		rot.row1.x =  0; rot.row1.y = 1;
		rot.row2.x = -1; rot.row2.y = 0;
		//Matrix_RotateZ(&rot, 90 * MATH_DEG2RAD);
		//Matrix_Mul(&_proj, &_proj, &rot); // TODO avoid Matrix_Mul ??
		Matrix_Mul(&_proj, matrix, &rot); // TODO avoid Matrix_Mul ?
	}

	UpdateMVP();
	DirtyUniform(UNI_MVP_MATRIX);
	ReloadUniforms();
}*/

void Gfx_LoadIdentityMatrix(MatrixType type) {
	Gfx_LoadMatrix(type, &Matrix_Identity);
}

void Gfx_EnableTextureOffset(float x, float y) {
	texOffset  = true;
	texOffsetX = x;
	texOffsetY = y;
	
	shaders[2].uniforms |= UNI_TEX_OFFSETS;
	SwitchProgram();
}
void Gfx_DisableTextureOffset(void) {
	texOffset  = false;
	SwitchProgram();
}



/*########################################################################################################################*
*---------------------------------------------------------Drawing---------------------------------------------------------*
*#########################################################################################################################*/
cc_bool Gfx_WarnIfNecessary(void) { return false; }

static void UpdateAttribFormat(VertexFormat fmt) {
	C3D_AttrInfo* attrInfo = C3D_GetAttrInfo();
	AttrInfo_Init(attrInfo);
	
	AttrInfo_AddLoader(attrInfo, 0, GPU_FLOAT, 3);         // in_pos
	AttrInfo_AddLoader(attrInfo, 1, GPU_UNSIGNED_BYTE, 4); // in_col
	if (fmt == VERTEX_FORMAT_TEXTURED) {
		AttrInfo_AddLoader(attrInfo, 2, GPU_FLOAT, 2); // in_tex
	}
}

static void UpdateTexEnv(VertexFormat fmt) {
	int func, source;
	
	if (fmt == VERTEX_FORMAT_TEXTURED) {
		// Configure the first fragment shading substage to blend the texture color with
  		// the vertex color (calculated by the vertex shader using a lighting algorithm)
  		// See https://www.opengl.org/sdk/docs/man2/xhtml/glTexEnv.xml for more insight	
  		source = GPU_TEVSOURCES(GPU_TEXTURE0, GPU_PRIMARY_COLOR, 0);
 		func   = GPU_MODULATE;
 	} else {
 		// Configure the first fragment shading substage to just pass through the vertex color
		// See https://www.opengl.org/sdk/docs/man2/xhtml/glTexEnv.xml for more insight
		source = GPU_TEVSOURCES(GPU_PRIMARY_COLOR, 0, 0);
		func   = GPU_REPLACE;
 	}

	GPUCMD_AddWrite(GPUREG_TEXENV0_SOURCE,   source | (source << 16));
	GPUCMD_AddWrite(GPUREG_TEXENV0_COMBINER, func   | (func   << 16));
}

void Gfx_SetVertexFormat(VertexFormat fmt) {
	if (fmt == gfx_format) return;
	gfx_format = fmt;
	gfx_stride = strideSizes[fmt];
	
	SwitchProgram();
	UpdateAttribFormat(fmt);
	UpdateTexEnv(fmt);
}

void Gfx_DrawVb_Lines(int verticesCount) {
	/* TODO */
}

static void SetVertexSource(int startVertex) {
	// https://github.com/devkitPro/citro3d/issues/47
	// "Fyi the permutation specifies the order in which the attributes are stored in the buffer, LSB first. So 0x210 indicates attributes 0, 1 & 2."
	const void* data;
	int stride, attribs, permutation;

	if (gfx_format == VERTEX_FORMAT_TEXTURED) {
		data    = (struct VertexTextured*)gfx_vertices + startVertex;
		stride  = SIZEOF_VERTEX_TEXTURED;
		attribs = 3;
		permutation = 0x210;
	} else {
		data    = (struct VertexColoured*)gfx_vertices + startVertex;
		stride  = SIZEOF_VERTEX_COLOURED;
		attribs = 2;
		permutation = 0x10;
	}

	u32 pa = osConvertVirtToPhys(data);
	u32 args[3]; // GPUREG_ATTRIBBUFFER0_OFFSET, GPUREG_ATTRIBBUFFER0_CONFIG1, GPUREG_ATTRIBBUFFER0_CONFIG2

	args[0] = pa - BUFFER_BASE_PADDR;
	args[1] = permutation;
	args[2] = (stride << 16) | (attribs << 28);

	GPUCMD_AddIncrementalWrites(GPUREG_ATTRIBBUFFER0_OFFSET, args, 3);
	// NOTE: Can't use GPUREG_VERTEX_OFFSET, it only works when drawing non-indexed arrays
}

void Gfx_DrawVb_IndexedTris_Range(int verticesCount, int startVertex) {
	SetVertexSource(startVertex);
	C3D_DrawElements(GPU_TRIANGLES, ICOUNT(verticesCount));
}

void Gfx_DrawVb_IndexedTris(int verticesCount) {
	SetVertexSource(0);
	C3D_DrawElements(GPU_TRIANGLES, ICOUNT(verticesCount));
}

void Gfx_DrawIndexedTris_T2fC4b(int verticesCount, int startVertex) {
	SetVertexSource(startVertex);
	C3D_DrawElements(GPU_TRIANGLES, ICOUNT(verticesCount));
}

// TODO: TEMP HACK !!
void Gfx_Draw2DFlat(int x, int y, int width, int height, PackedCol color) {
	struct VertexColoured v1, v2, v3, v4;
	v1.x = (float)x;           v1.y = (float)y;
	v2.x = (float)(x + width); v2.y = (float)y;
	v3.x = (float)(x + width); v3.y = (float)(y + height);
	v4.x = (float)x;           v4.y = (float)(y + height);
	Gfx_SetVertexFormat(VERTEX_FORMAT_COLOURED);
	C3D_ImmDrawBegin(GPU_TRIANGLES);
		C3D_ImmSendAttrib(v1.x, v1.y, 0.0f, 1.0f);
		C3D_ImmSendAttrib(PackedCol_R(color), PackedCol_G(color), PackedCol_B(color), PackedCol_A(color));
		C3D_ImmSendAttrib(v2.x, v2.y, 0.0f, 1.0f);
		C3D_ImmSendAttrib(PackedCol_R(color), PackedCol_G(color), PackedCol_B(color), PackedCol_A(color));
		C3D_ImmSendAttrib(v3.x, v3.y, 0.0f, 1.0f);
		C3D_ImmSendAttrib(PackedCol_R(color), PackedCol_G(color), PackedCol_B(color), PackedCol_A(color));
		C3D_ImmSendAttrib(v3.x, v3.y, 0.0f, 1.0f);
		C3D_ImmSendAttrib(PackedCol_R(color), PackedCol_G(color), PackedCol_B(color), PackedCol_A(color));
		C3D_ImmSendAttrib(v4.x, v4.y, 0.0f, 1.0f);
		C3D_ImmSendAttrib(PackedCol_R(color), PackedCol_G(color), PackedCol_B(color), PackedCol_A(color));
		C3D_ImmSendAttrib(v1.x, v1.y, 0.0f, 1.0f);
		C3D_ImmSendAttrib(PackedCol_R(color), PackedCol_G(color), PackedCol_B(color), PackedCol_A(color));
	C3D_ImmDrawEnd();
}

void Gfx_Draw2DGradient(int x, int y, int width, int height, PackedCol top, PackedCol bottom) {
	struct VertexColoured v1, v2, v3, v4;
	v1.x = (float)x;           v1.y = (float)y;
	v2.x = (float)(x + width); v2.y = (float)y;
	v3.x = (float)(x + width); v3.y = (float)(y + height);
	v4.x = (float)x;           v4.y = (float)(y + height);
	Gfx_SetVertexFormat(VERTEX_FORMAT_COLOURED);
	C3D_ImmDrawBegin(GPU_TRIANGLES);
		C3D_ImmSendAttrib(v1.x, v1.y, 0.0f, 1.0f);
		C3D_ImmSendAttrib(PackedCol_R(top), PackedCol_G(top), PackedCol_B(top), PackedCol_A(top));
		C3D_ImmSendAttrib(v2.x, v2.y, 0.0f, 1.0f);
		C3D_ImmSendAttrib(PackedCol_R(top), PackedCol_G(top), PackedCol_B(top), PackedCol_A(top));
		C3D_ImmSendAttrib(v3.x, v3.y, 0.0f, 1.0f);
		C3D_ImmSendAttrib(PackedCol_R(bottom), PackedCol_G(bottom), PackedCol_B(bottom), PackedCol_A(bottom));
		C3D_ImmSendAttrib(v3.x, v3.y, 0.0f, 1.0f);
		C3D_ImmSendAttrib(PackedCol_R(bottom), PackedCol_G(bottom), PackedCol_B(bottom), PackedCol_A(bottom));
		C3D_ImmSendAttrib(v4.x, v4.y, 0.0f, 1.0f);
		C3D_ImmSendAttrib(PackedCol_R(bottom), PackedCol_G(bottom), PackedCol_B(bottom), PackedCol_A(bottom));
		C3D_ImmSendAttrib(v1.x, v1.y, 0.0f, 1.0f);
		C3D_ImmSendAttrib(PackedCol_R(top), PackedCol_G(top), PackedCol_B(top), PackedCol_A(top));
	C3D_ImmDrawEnd();
}

void Gfx_Draw2DTexture(const struct Texture* tex, PackedCol color) {
	struct VertexTextured v[4];
	struct VertexTextured* ptr = v;
	Gfx_Make2DQuad(tex, color, &ptr);
	Gfx_SetVertexFormat(VERTEX_FORMAT_TEXTURED);
	C3D_ImmDrawBegin(GPU_TRIANGLES);
		C3D_ImmSendAttrib(v[0].x, v[0].y, 0.0f, 1.0f);
		C3D_ImmSendAttrib(PackedCol_R(color), PackedCol_G(color), PackedCol_B(color), PackedCol_A(color));
		C3D_ImmSendAttrib(v[0].U, v[0].V, 0.0f, 0.0f);
		C3D_ImmSendAttrib(v[1].x, v[1].y, 0.0f, 1.0f);
		C3D_ImmSendAttrib(PackedCol_R(color), PackedCol_G(color), PackedCol_B(color), PackedCol_A(color));
		C3D_ImmSendAttrib(v[1].U, v[1].V, 0.0f, 0.0f);
		C3D_ImmSendAttrib(v[2].x, v[2].y, 0.0f, 1.0f);
		C3D_ImmSendAttrib(PackedCol_R(color), PackedCol_G(color), PackedCol_B(color), PackedCol_A(color));
		C3D_ImmSendAttrib(v[2].U, v[2].V, 0.0f, 0.0f);
		C3D_ImmSendAttrib(v[2].x, v[2].y, 0.0f, 1.0f);
		C3D_ImmSendAttrib(PackedCol_R(color), PackedCol_G(color), PackedCol_B(color), PackedCol_A(color));
		C3D_ImmSendAttrib(v[2].U, v[2].V, 0.0f, 0.0f);
		C3D_ImmSendAttrib(v[3].x, v[3].y, 0.0f, 1.0f);
		C3D_ImmSendAttrib(PackedCol_R(color), PackedCol_G(color), PackedCol_B(color), PackedCol_A(color));
		C3D_ImmSendAttrib(v[3].U, v[3].V, 0.0f, 0.0f);
		C3D_ImmSendAttrib(v[0].x, v[0].y, 0.0f, 1.0f);
		C3D_ImmSendAttrib(PackedCol_R(color), PackedCol_G(color), PackedCol_B(color), PackedCol_A(color));
		C3D_ImmSendAttrib(v[0].U, v[0].V, 0.0f, 0.0f);
	C3D_ImmDrawEnd();
}
#endif
