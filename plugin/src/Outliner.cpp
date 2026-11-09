// STLT-OUTLINER: outlines drawn inside every slice of a view, so they never bleed into
// neighbours. Built twice: as an effect that draws over its input, and, with
// STLT_OUTLINER_SOURCE defined, as a source that needs no input at all, so the outlines can be
// used as a clip of their own or as a mask for STLT-MASK. Static outlines can feather inward; animated ones become tapered lines that
// run around each slice's edge, driven by Phase (animate it in Resolume).
#include <FFGLSDK.h>
#include <memory>
#include "ChaseDocument.h"
#include "Outline.h"
#include "PluginCommon.h"
using namespace ffglex;
using namespace stilletchaser;

namespace
{
enum ParamID
{
	PID_VIEW,
	PID_RED,
	PID_GREEN,
	PID_BLUE,
	PID_WIDTH,
	PID_FEATHER,
	PID_ANIMATE,
	PID_PHASE,
	PID_FAN,
	PID_RANDOMISE,
	PID_LENGTH,
	PID_COUNT_LINES,
	PID_FLIP,
	PID_ODD_EVEN,
	PID_ENVELOPE,
	PID_SHOW_ORIGINAL,
	PID_LIT_ONLY,
	PID_COUNT
};

const char copyFragmentShader[] = R"(#version 410 core
uniform sampler2D inputTexture;
uniform vec2 maxUV;
in vec2 uv;
out vec4 fragColor;
void main()
{
	fragColor = texture( inputTexture, uv * maxUV );
}
)";

const char copyVertexShader[] = R"(#version 410 core
layout( location = 0 ) in vec4 vPosition;
layout( location = 1 ) in vec2 vUV;
out vec2 uv;
void main()
{
	gl_Position = vPosition;
	uv          = vUV;
}
)";
}// namespace

class Outliner : public StilletchaserPlugin
{
public:
	Outliner()
	{
#ifdef STLT_OUTLINER_SOURCE
		// A source has nothing underneath it, so there is nothing to show through or follow.
		SetMinInputs( 0 );
		SetMaxInputs( 0 );
#else
		SetMinInputs( 1 );
		SetMaxInputs( 1 );
#endif
		SetOptionParamInfo( PID_VIEW, "View", 1, 0.f );
		SetParamElementInfo( PID_VIEW, 0, "All", 0.f );
		SetParamInfo( PID_RED, "Red", FF_TYPE_RED, 1.f );
		SetParamInfo( PID_GREEN, "Green", FF_TYPE_GREEN, 1.f );
		SetParamInfo( PID_BLUE, "Blue", FF_TYPE_BLUE, 1.f );
		SetRangedParamInfo( PID_WIDTH, "Width", FF_TYPE_STANDARD, 10.f, 1.f, 300.f );
		SetRangedParamInfo( PID_FEATHER, "Feather", FF_TYPE_STANDARD, 0.f, 0.f, 500.f );
		SetParamInfo( PID_ANIMATE, "Animate", FF_TYPE_BOOLEAN, false );
		SetParamInfo( PID_PHASE, "Phase", FF_TYPE_STANDARD, 0.f );
		SetParamInfo( PID_FAN, "Fan", FF_TYPE_STANDARD, 0.f );
		SetParamInfo( PID_RANDOMISE, "Randomise", FF_TYPE_STANDARD, 0.f );
		SetRangedParamInfo( PID_LENGTH, "Length", FF_TYPE_STANDARD, 0.25f, 0.01f, 1.f );
		SetRangedParamInfo( PID_COUNT_LINES, "Count", FF_TYPE_INTEGER, 1.f, 1.f, (float)OutlineRenderer::MAX_LINES );
		SetParamInfo( PID_FLIP, "Flip Direction", FF_TYPE_BOOLEAN, false );
		SetParamInfo( PID_ODD_EVEN, "Odd/Even", FF_TYPE_BOOLEAN, false );
		SetOptionParamInfo( PID_ENVELOPE, "Envelope", 1, 0.f );
		SetParamElementInfo( PID_ENVELOPE, 0, "Taper", 0.f );
		SetParamInfo( PID_SHOW_ORIGINAL, "Show Original", FF_TYPE_BOOLEAN, true );
		SetParamInfo( PID_LIT_ONLY, "Lit Slices Only", FF_TYPE_BOOLEAN, false );
		for( int id = PID_PHASE; id <= PID_ENVELOPE; ++id )
			SetParamGroup( id, "Animation" );

		values[ PID_VIEW ]  = 0.f;
		values[ PID_RED ]   = values[ PID_GREEN ] = values[ PID_BLUE ] = 1.f;
		values[ PID_WIDTH ] = 10.f;
		values[ PID_FEATHER ] = values[ PID_ANIMATE ] = values[ PID_PHASE ] = values[ PID_FAN ] = values[ PID_RANDOMISE ] = 0.f;
		values[ PID_LENGTH ]      = 0.25f;
		values[ PID_COUNT_LINES ] = 1.f;
		values[ PID_FLIP ] = values[ PID_ODD_EVEN ] = values[ PID_ENVELOPE ] = values[ PID_LIT_ONLY ] = 0.f;
		values[ PID_SHOW_ORIGINAL ] = 1.f;

		doc        = DocumentWatcher::Instance().Current();
		docVersion = DocumentWatcher::Instance().Version();
		RefreshMenus( false );
	}

	FFResult InitGL( const FFGLViewportStruct* vp ) override
	{
		if( !outlines.InitGL() || !copyShader.Compile( copyVertexShader, copyFragmentShader ) || !quad.Initialise() )
		{
			DeInitGL();
			return FF_FAIL;
		}
		return CFFGLPlugin::InitGL( vp );
	}

	FFResult DeInitGL() override
	{
		outlines.DeInitGL();
		copyShader.FreeGLResources();
		quad.Release();
		return FF_SUCCESS;
	}

	FFResult SetFloatParameter( unsigned int index, float value ) override
	{
		if( index >= PID_COUNT )
			return FF_FAIL;
		values[ index ] = value;
		if( index == PID_ANIMATE )
			UpdateVisibility( true );
		return FF_SUCCESS;
	}

	float GetFloatParameter( unsigned int index ) override
	{
		return index < PID_COUNT ? values[ index ] : 0.f;
	}

	const char* GetShortName() override
	{
#ifdef STLT_OUTLINER_SOURCE
		static const char* sname = "STOutSrc";
#else
		static const char* sname = "STOutline";
#endif
		return sname;
	}

	FFResult ProcessOpenGL( ProcessOpenGLStruct* pGL ) override
	{
#ifdef STLT_OUTLINER_SOURCE
		const FFGLTextureStruct* input = nullptr;
#else
		if( pGL->numInputTextures < 1 || pGL->inputTextures[ 0 ] == nullptr )
			return FF_FAIL;
		const FFGLTextureStruct* input = pGL->inputTextures[ 0 ];
#endif
		DocumentWatcher& watcher       = DocumentWatcher::Instance();
		doc                            = watcher.Current();
		if( watcher.Version() != docVersion )
		{
			docVersion = watcher.Version();
			RefreshMenus( true );
		}

		glClearColor( 0.f, 0.f, 0.f, 0.f );
		glClear( GL_COLOR_BUFFER_BIT );
		if( input != nullptr && values[ PID_SHOW_ORIGINAL ] > 0.5f )
		{
			ScopedShaderBinding copyBinding( copyShader.GetGLID() );
			ScopedSamplerActivation activateSampler( 0 );
			Scoped2DTextureBinding textureBinding( input->Handle );
			FFGLTexCoords maxCoords = GetMaxGLTexCoords( *input );
			copyShader.Set( "inputTexture", 0 );
			copyShader.Set( "maxUV", maxCoords.s, maxCoords.t );
			quad.Draw();
		}
		if( doc->slices.empty() )
			return FF_SUCCESS;

		const View& view = doc->views[ Option( values[ PID_VIEW ], (int)doc->views.size() ) ];
		std::vector< float > strength( doc->slices.size() );
		for( size_t s = 0; s < strength.size(); ++s )
			strength[ s ] = view.includes[ s ] ? 1.f : 0.f;
		const bool litOnly = input != nullptr && values[ PID_LIT_ONLY ] > 0.5f;
		outlines.Draw( *doc, Style(), strength, litOnly ? input : nullptr );
		return FF_SUCCESS;
	}

private:
	OutlineStyle Style() const
	{
		OutlineStyle style;
		style.red       = values[ PID_RED ];
		style.green     = values[ PID_GREEN ];
		style.blue      = values[ PID_BLUE ];
		style.width     = values[ PID_WIDTH ];
		style.feather   = values[ PID_FEATHER ];
		style.animate   = values[ PID_ANIMATE ] > 0.5f;
		style.phase     = values[ PID_PHASE ];
		style.fan       = values[ PID_FAN ];
		style.randomise = values[ PID_RANDOMISE ];
		style.length    = values[ PID_LENGTH ];
		style.count     = (int)std::lround( values[ PID_COUNT_LINES ] );
		style.flip      = values[ PID_FLIP ] > 0.5f;
		style.oddEven   = values[ PID_ODD_EVEN ] > 0.5f;
		style.envelope  = ChosenEnvelope( *doc, values[ PID_ENVELOPE ] );
		return style;
	}

	void RefreshMenus( bool raiseEvents )
	{
		const auto views     = ViewNames( *doc );
		const auto envelopes = EnvelopeNames( *doc, "Taper" );
		SetParamElements( PID_VIEW, views, Indices( views.size() ), raiseEvents );
		SetParamElements( PID_ENVELOPE, envelopes, Indices( envelopes.size() ), raiseEvents );
		UpdateVisibility( raiseEvents );
	}

	// Feather is for static outlines; the animation controls only matter when animating.
	void UpdateVisibility( bool raiseEvents )
	{
		const bool animate = values[ PID_ANIMATE ] > 0.5f;
#ifdef STLT_OUTLINER_SOURCE
		SetParamVisibility( PID_SHOW_ORIGINAL, false, raiseEvents );
		SetParamVisibility( PID_LIT_ONLY, false, raiseEvents );
#endif
		SetParamVisibility( PID_FEATHER, !animate, raiseEvents );
		for( int id = PID_PHASE; id <= PID_ENVELOPE; ++id )
			SetParamVisibility( id, animate, raiseEvents );
	}

	OutlineRenderer outlines;
	FFGLShader copyShader;
	FFGLScreenQuad quad;
	std::shared_ptr< const ChaseDocument > doc;
	uint64_t docVersion = 0;
	float values[ PID_COUNT ];
};

#ifdef STLT_OUTLINER_SOURCE
static CFFGLPluginInfo PluginInfo(
	PluginFactory< Outliner >,
	"STOS",                 // Plugin unique ID
	"STLT-OUTLINER SOURCE", // Plugin name
	2,                      // API major version number
	1,                      // API minor version number
	1,                      // Plugin major version number
	0,                      // Plugin minor version number
	FF_SOURCE,
	"Slice outlines as a clip of their own, with no input needed",
	"Stilletchaser" );
#else
static CFFGLPluginInfo PluginInfo(
	PluginFactory< Outliner >,
	"STOL",          // Plugin unique ID
	"STLT-OUTLINER", // Plugin name
	2,               // API major version number
	1,               // API minor version number
	1,               // Plugin major version number
	2,               // Plugin minor version number
	FF_EFFECT,
	"Outlines inside Advanced Output slices, static or running around their edges",
	"Stilletchaser" );
#endif
