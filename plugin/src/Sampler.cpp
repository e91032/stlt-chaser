// STLT-SAMPLER: reads the content lying under each slice and fills that slice with it as a flat
// colour, so a video drives a wall of lights rather than being shown on it. Centre takes the one
// pixel at the slice's middle, Average the whole area, and Blend keeps a soft gradient across the
// slice instead of one flat colour.
#include <FFGLSDK.h>
#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>
#include "ChaseDocument.h"
#include "PluginCommon.h"
using namespace ffglex;
using namespace stilletchaser;

namespace
{
enum ParamID
{
	PID_VIEW,
	PID_SAMPLE_MODE,
	PID_COLOUR_MODE,
	PID_SPREAD,
	PID_GAIN,
	PID_SHOW_ORIGINAL,
	PID_COUNT
};

enum SampleMode
{
	SAMPLE_CENTRE,
	SAMPLE_AVERAGE,
	SAMPLE_BLEND,
};

enum ColourMode
{
	COLOUR_FULL,
	COLOUR_LUMINANCE,
};

const int FLOATS_PER_VERTEX = 4;// compPos.xy, contentUV.xy

const char vertexShaderBody[] = R"(
layout( location = 0 ) in vec2 compPos;
layout( location = 1 ) in vec2 contentUV;
out vec2 uv;
void main()
{
	gl_Position = compToClip( compPos );
	uv          = contentUV;
}
)";

// Centre reads one point, Average walks a grid over the slice's own area, and Blend does the same
// but around the fragment, over a window shrunk by Spread, so the colour still drifts across it.
const char fragmentShaderBody[] = R"(#version 410 core
uniform sampler2D inputTexture;
uniform vec2 maxUV;
uniform vec2 uvMin;
uniform vec2 uvMax;
uniform vec2 centreUV;
uniform int sampleMode;
uniform int colourMode;
uniform float spread;
uniform float gain;
in vec2 uv;
out vec4 fragColor;

const int TAPS = 6;

vec3 Read( vec2 at )
{
	return texture( inputTexture, clamp( at, vec2( 0.0 ), vec2( 1.0 ) ) * maxUV ).rgb;
}

vec3 Box( vec2 centre, vec2 halfSize )
{
	vec3 total = vec3( 0.0 );
	for( int y = 0; y < TAPS; ++y )
		for( int x = 0; x < TAPS; ++x )
		{
			vec2 offset = ( vec2( float( x ), float( y ) ) + 0.5 ) / float( TAPS ) * 2.0 - 1.0;
			total += Read( centre + offset * halfSize );
		}
	return total / float( TAPS * TAPS );
}

void main()
{
	vec2 halfSize = abs( uvMax - uvMin ) * 0.5;
	vec3 rgb;
	if( sampleMode == 0 )
		rgb = Read( centreUV );
	else if( sampleMode == 1 )
		rgb = Box( ( uvMin + uvMax ) * 0.5, halfSize );
	else
		rgb = Box( uv, max( halfSize * spread, vec2( 1.0 / 4096.0 ) ) );

	if( colourMode == 1 )
		rgb = vec3( dot( rgb, vec3( 0.2126, 0.7152, 0.0722 ) ) );
	fragColor = vec4( clamp( rgb * gain, 0.0, 1.0 ), 1.0 );
}
)";

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

class Sampler : public StilletchaserPlugin
{
public:
	Sampler()
	{
		SetMinInputs( 1 );
		SetMaxInputs( 1 );

		SetOptionParamInfo( PID_VIEW, "View", 1, 0.f );
		SetParamElementInfo( PID_VIEW, 0, "All", 0.f );

		SetOptionParamInfo( PID_SAMPLE_MODE, "Sample Mode", 3, (float)SAMPLE_AVERAGE );
		SetParamElementInfo( PID_SAMPLE_MODE, SAMPLE_CENTRE, "Centre", (float)SAMPLE_CENTRE );
		SetParamElementInfo( PID_SAMPLE_MODE, SAMPLE_AVERAGE, "Average", (float)SAMPLE_AVERAGE );
		SetParamElementInfo( PID_SAMPLE_MODE, SAMPLE_BLEND, "Blend", (float)SAMPLE_BLEND );

		SetOptionParamInfo( PID_COLOUR_MODE, "Colour Mode", 2, (float)COLOUR_FULL );
		SetParamElementInfo( PID_COLOUR_MODE, COLOUR_FULL, "Colour", (float)COLOUR_FULL );
		SetParamElementInfo( PID_COLOUR_MODE, COLOUR_LUMINANCE, "Luminance", (float)COLOUR_LUMINANCE );

		SetRangedParamInfo( PID_SPREAD, "Blend Spread", FF_TYPE_STANDARD, 0.5f, 0.05f, 2.f );
		SetRangedParamInfo( PID_GAIN, "Gain", FF_TYPE_STANDARD, 1.f, 0.f, 4.f );
		SetParamInfo( PID_SHOW_ORIGINAL, "Show Original", FF_TYPE_BOOLEAN, false );

		values[ PID_VIEW ]          = 0.f;
		values[ PID_SAMPLE_MODE ]   = (float)SAMPLE_AVERAGE;
		values[ PID_COLOUR_MODE ]   = (float)COLOUR_FULL;
		values[ PID_SPREAD ]        = 0.5f;
		values[ PID_GAIN ]          = 1.f;
		values[ PID_SHOW_ORIGINAL ] = 0.f;
		doc                         = DocumentWatcher::Instance().Current();
	}

	FFResult InitGL( const FFGLViewportStruct* vp ) override
	{
		const std::string vertex = std::string( CompositionVertexHeader() ) + vertexShaderBody;
		if( !shader.Compile( vertex.c_str(), fragmentShaderBody ) ||
			!copyShader.Compile( copyVertexShader, copyFragmentShader ) ||
			!quad.Initialise() ||
			!mesh.Init( { 2, 2 } ) )
		{
			DeInitGL();
			return FF_FAIL;
		}
		RefreshMenus( false );
		return FF_SUCCESS;
	}

	FFResult DeInitGL() override
	{
		shader.FreeGLResources();
		copyShader.FreeGLResources();
		quad.Release();
		mesh.Release();
		return FF_SUCCESS;
	}

	FFResult SetFloatParameter( unsigned int index, float value ) override
	{
		if( index >= PID_COUNT )
			return FF_FAIL;
		values[ index ] = value;
		if( index == PID_SAMPLE_MODE )
			UpdateVisibility( true );
		return FF_SUCCESS;
	}

	float GetFloatParameter( unsigned int index ) override
	{
		return index < PID_COUNT ? values[ index ] : 0.f;
	}

	const char* GetShortName() override
	{
		static const char* sname = "STSample";
		return sname;
	}

	FFResult ProcessOpenGL( ProcessOpenGLStruct* pGL ) override
	{
		if( pGL->numInputTextures < 1 || pGL->inputTextures[ 0 ] == nullptr )
			return FF_FAIL;
		const FFGLTextureStruct& input = *pGL->inputTextures[ 0 ];
		DocumentWatcher& watcher       = DocumentWatcher::Instance();
		doc                            = watcher.Current();
		if( watcher.Version() != docVersion )
		{
			docVersion = watcher.Version();
			RefreshMenus( true );
		}

		glClearColor( 0.f, 0.f, 0.f, 0.f );
		glClear( GL_COLOR_BUFFER_BIT );
		const FFGLTexCoords maxCoords = GetMaxGLTexCoords( input );
		if( values[ PID_SHOW_ORIGINAL ] > 0.5f )
		{
			ScopedShaderBinding copyBinding( copyShader.GetGLID() );
			ScopedSamplerActivation activateSampler( 0 );
			Scoped2DTextureBinding textureBinding( input.Handle );
			copyShader.Set( "inputTexture", 0 );
			copyShader.Set( "maxUV", maxCoords.s, maxCoords.t );
			quad.Draw();
		}
		if( doc->slices.empty() || doc->views.empty() )
			return FF_SUCCESS;

		BuildVertices( *doc );
		if( vertices.empty() )
			return FF_SUCCESS;
		mesh.Upload( vertices );

		const View& view = doc->views[ Option( values[ PID_VIEW ], (int)doc->views.size() ) ];
		ScopedShaderBinding binding( shader.GetGLID() );
		ScopedSamplerActivation activateSampler( 0 );
		Scoped2DTextureBinding textureBinding( input.Handle );
		shader.Set( "inputTexture", 0 );
		shader.Set( "maxUV", maxCoords.s, maxCoords.t );
		shader.Set( "sampleMode", Option( values[ PID_SAMPLE_MODE ], 3 ) );
		shader.Set( "colourMode", Option( values[ PID_COLOUR_MODE ], 2 ) );
		shader.Set( "spread", std::clamp( values[ PID_SPREAD ], 0.05f, 2.f ) );
		shader.Set( "gain", std::clamp( values[ PID_GAIN ], 0.f, 4.f ) );

		glBindVertexArray( mesh.VAO() );
		for( size_t s = 0; s < draws.size(); ++s )
		{
			if( s < view.includes.size() && !view.includes[ s ] )
				continue;
			shader.Set( "uvMin", draws[ s ].uvMin.x, draws[ s ].uvMin.y );
			shader.Set( "uvMax", draws[ s ].uvMax.x, draws[ s ].uvMax.y );
			shader.Set( "centreUV", draws[ s ].centre.x, draws[ s ].centre.y );
			glDrawArrays( GL_TRIANGLES, draws[ s ].first, draws[ s ].count );
		}
		glBindVertexArray( 0 );
		return FF_SUCCESS;
	}

private:
	// Each slice's triangles plus the patch of content it covers, in texture coordinates.
	struct SliceDraw
	{
		GLint first;
		GLsizei count;
		Vec2 uvMin, uvMax, centre;
	};

	// Sampling always reads the content lying under the slice, so the geometry is built in Mask
	// scaling whatever the slice's own shape is.
	void BuildVertices( const ChaseDocument& d )
	{
		vertices.clear();
		draws.clear();
		for( const Slice& slice : d.slices )
		{
			SliceDraw draw{ (GLint)( vertices.size() / FLOATS_PER_VERTEX ), (GLsizei)slice.indices.size(), {}, {}, {} };
			Vec2 lo{ 1.f, 1.f }, hi{ 0.f, 0.f };
			for( uint16_t i : slice.indices )
			{
				const Vec2 p  = slice.points[ i ];
				const Vec2 uv = ContentUV( d, slice, p, SCALING_MASK, 1.f );
				lo            = { std::min( lo.x, uv.x ), std::min( lo.y, uv.y ) };
				hi            = { std::max( hi.x, uv.x ), std::max( hi.y, uv.y ) };
				vertices.insert( vertices.end(), { p.x, p.y, uv.x, uv.y } );
			}
			const Vec2 c = SliceCentre( slice );
			draw.uvMin   = lo;
			draw.uvMax   = hi;
			draw.centre  = { c.x, 1.f - c.y };
			draws.push_back( draw );
		}
	}

	void RefreshMenus( bool raiseEvents )
	{
		const auto views = ViewNames( *doc );
		SetParamElements( PID_VIEW, views, Indices( views.size() ), raiseEvents );
		UpdateVisibility( raiseEvents );
	}

	// Blend Spread only means anything in Blend.
	void UpdateVisibility( bool raiseEvents )
	{
		SetParamVisibility( PID_SPREAD, Option( values[ PID_SAMPLE_MODE ], 3 ) == SAMPLE_BLEND, raiseEvents );
	}

	FFGLShader shader, copyShader;
	FFGLScreenQuad quad;
	FloatVertexBuffer mesh;
	std::vector< float > vertices;
	std::vector< SliceDraw > draws;
	std::shared_ptr< const ChaseDocument > doc;
	uint64_t docVersion = 0;
	float values[ PID_COUNT ];
};

static CFFGLPluginInfo PluginInfo(
	PluginFactory< Sampler >,
	"STSA",         // Plugin unique ID
	"STLT-SAMPLER", // Plugin name
	2,              // API major version number
	1,              // API minor version number
	1,              // Plugin major version number
	0,              // Plugin minor version number
	FF_EFFECT,
	"Fills each Advanced Output slice with the colour of the content under it",
	"Stilletchaser" );
