// STLT-TILER: fills every slice of a view with a grid of tiles and runs a pattern across them,
// so a plain LED wall gets content of its own without any footage. The tiles are laid out in each
// slice's own frame, in composition pixels, so they stay square whatever shape the slice is.
#include <FFGLSDK.h>
#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
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
	PID_TILE_SIZE,
	PID_MODE,
	PID_PHASE,
	PID_LENGTH,
	PID_SEED,
	PID_RANDOMISE,
	PID_FAN,
	PID_DIRECTION,
	PID_SHAPE,
	PID_RADIUS,
	PID_SOFTNESS,
	PID_EDGE_ONLY,
	PID_RED,
	PID_GREEN,
	PID_BLUE,
	PID_GRID_SIZE,
	PID_GRID_RED,
	PID_GRID_GREEN,
	PID_GRID_BLUE,
	PID_SHOW_ORIGINAL,
	PID_COUNT
};

enum Mode
{
	MODE_MINIMAL,
	MODE_RAINBOW,
	MODE_TEXTURE,
	MODE_MATRIX,
};

enum Direction
{
	DIR_RIGHT,
	DIR_LEFT,
	DIR_DOWN,
	DIR_UP,
};

const int FLOATS_PER_VERTEX = 6;// compPos.xy, contentUV.xy, sliceLocal.xy

const char vertexShaderBody[] = R"(
layout( location = 0 ) in vec2 compPos;
layout( location = 1 ) in vec2 contentUV;
layout( location = 2 ) in vec2 sliceLocal;
out vec2 uv;
out vec2 local;
void main()
{
	gl_Position = compToClip( compPos );
	uv          = contentUV;
	local       = sliceLocal;
}
)";

// Every fragment works out which tile it belongs to, how far along the travel that tile sits, and
// how bright the running head leaves it. Shape then carves the tile down to a square or a dot.
const char fragmentShaderBody[] = R"(#version 410 core
uniform sampler2D inputTexture;
uniform vec2 maxUV;
uniform vec2 sliceSize;  // The slice's size in composition pixels.
// The slice's frame in texture coordinates, so a tile can read one colour for the whole tile.
uniform vec2 uvOrigin;
uniform vec2 uvAxisX;
uniform vec2 uvAxisY;
uniform float sliceFan;  // This slice's share of Fan, already worked out on the CPU.
uniform float tileSize;
uniform int mode;
uniform float phase;
uniform float runLength;
uniform float seed;
uniform float randomise;
uniform int direction;
uniform int shape;
uniform float radius;
uniform float softness;
uniform int edgeOnly;
uniform vec3 colour;
uniform float gridSize;
uniform vec3 gridColour;
in vec2 uv;
in vec2 local;
out vec4 fragColor;

float Hash( vec2 at )
{
	return fract( sin( dot( at + seed, vec2( 127.1, 311.7 ) ) ) * 43758.5453 );
}

vec3 Hue( float h )
{
	vec3 k = fract( vec3( h ) + vec3( 0.0, 2.0 / 3.0, 1.0 / 3.0 ) );
	return clamp( abs( k * 6.0 - 3.0 ) - 1.0, 0.0, 1.0 );
}

void main()
{
	// The tile grid, counted in composition pixels so tiles stay square in any slice.
	vec2 tiles  = max( floor( sliceSize / max( tileSize, 1.0 ) ), vec2( 1.0 ) );
	vec2 scaled = local * tiles;
	vec2 cell   = floor( scaled );
	vec2 within = fract( scaled ) - 0.5;// -0.5 .. 0.5 inside the tile.

	// How far along the travel this tile sits, 0 at the edge the pattern enters from.
	float travel;
	if( direction == 0 )
		travel = ( cell.x + 0.5 ) / tiles.x;
	else if( direction == 1 )
		travel = 1.0 - ( cell.x + 0.5 ) / tiles.x;
	else if( direction == 2 )
		travel = ( cell.y + 0.5 ) / tiles.y;
	else
		travel = 1.0 - ( cell.y + 0.5 ) / tiles.y;

	// Matrix scatters whole columns rather than single tiles, which is what makes it rain.
	float noise = mode == 3 ? Hash( vec2( cell.x, 0.0 ) ) : Hash( cell );
	float head  = phase + sliceFan + randomise * noise;
	float x     = fract( head - travel );
	float run   = max( runLength, 0.001 );
	if( x > run )
		discard;
	float brightness = 1.0 - x / run;

	vec3 rgb;
	if( mode == 0 )
		rgb = colour;
	else if( mode == 1 )
		rgb = Hue( fract( travel + phase + noise * randomise ) ) * colour;
	else if( mode == 2 )
	{
		// One reading per tile, taken at its centre, so the content comes through as a mosaic.
		vec2 centreLocal = ( cell + 0.5 ) / tiles;
		vec2 tileUV      = uvOrigin + centreLocal.x * uvAxisX + centreLocal.y * uvAxisY;
		rgb              = texture( inputTexture, clamp( tileUV, 0.0, 1.0 ) * maxUV ).rgb;
	}
	else
	{
		// The head of a falling column runs white, the tail stays in the chosen colour.
		float tip = smoothstep( 0.85, 1.0, brightness );
		rgb       = mix( colour, vec3( 1.0 ), tip );
	}

	// The tile's shape: a square by Chebyshev distance, a dot by Euclidean.
	float dist = shape == 0 ? max( abs( within.x ), abs( within.y ) ) * 2.0 : clamp( length( within ) * 2.0, 0.0, 1.4142 );
	float soft = max( softness, 0.001 );
	float alpha;
	if( edgeOnly != 0 )
	{
		// A ring sitting on the radius rather than a filled tile.
		float band = abs( dist - radius );
		alpha      = 1.0 - smoothstep( 0.0, soft, band );
	}
	else
		alpha = 1.0 - smoothstep( radius - soft, radius, dist );

	// The grid is drawn on the slice itself, under the tiles, and ignores the running pattern.
	vec3 out_rgb = rgb * brightness * alpha;
	float out_a  = alpha * brightness;
	if( gridSize > 0.0 )
	{
		vec2 lines = fract( local * max( sliceSize / max( gridSize, 1.0 ), vec2( 1.0 ) ) );
		vec2 edge  = min( lines, 1.0 - lines );
		float on   = 1.0 - smoothstep( 0.0, 0.08, min( edge.x, edge.y ) );
		out_rgb    = mix( out_rgb, gridColour, on * ( 1.0 - out_a ) );
		out_a      = max( out_a, on );
	}
	if( out_a <= 0.0 )
		discard;
	fragColor = vec4( out_rgb, out_a );
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

class Tiler : public StilletchaserPlugin
{
public:
	Tiler()
	{
		SetMinInputs( 1 );
		SetMaxInputs( 1 );

		SetOptionParamInfo( PID_VIEW, "View", 1, 0.f );
		SetParamElementInfo( PID_VIEW, 0, "All", 0.f );
		SetRangedParamInfo( PID_TILE_SIZE, "Tile Size", FF_TYPE_STANDARD, 24.f, 2.f, 400.f );

		SetOptionParamInfo( PID_MODE, "Mode", 4, (float)MODE_MINIMAL );
		SetParamElementInfo( PID_MODE, MODE_MINIMAL, "Minimal", (float)MODE_MINIMAL );
		SetParamElementInfo( PID_MODE, MODE_RAINBOW, "Rainbow", (float)MODE_RAINBOW );
		SetParamElementInfo( PID_MODE, MODE_TEXTURE, "Texture", (float)MODE_TEXTURE );
		SetParamElementInfo( PID_MODE, MODE_MATRIX, "Matrix", (float)MODE_MATRIX );

		SetParamInfo( PID_PHASE, "Phase", FF_TYPE_STANDARD, 0.f );
		SetRangedParamInfo( PID_LENGTH, "Length", FF_TYPE_STANDARD, 0.35f, 0.01f, 1.f );
		SetRangedParamInfo( PID_SEED, "Seed", FF_TYPE_STANDARD, 0.f, 0.f, 100.f );
		SetParamInfo( PID_RANDOMISE, "Randomise", FF_TYPE_STANDARD, 0.f );
		SetParamInfo( PID_FAN, "Fan", FF_TYPE_STANDARD, 0.f );

		SetOptionParamInfo( PID_DIRECTION, "Direction", 4, (float)DIR_RIGHT );
		SetParamElementInfo( PID_DIRECTION, DIR_RIGHT, "Right", (float)DIR_RIGHT );
		SetParamElementInfo( PID_DIRECTION, DIR_LEFT, "Left", (float)DIR_LEFT );
		SetParamElementInfo( PID_DIRECTION, DIR_DOWN, "Down", (float)DIR_DOWN );
		SetParamElementInfo( PID_DIRECTION, DIR_UP, "Up", (float)DIR_UP );

		SetOptionParamInfo( PID_SHAPE, "Shape", 2, 0.f );
		SetParamElementInfo( PID_SHAPE, 0, "Square", 0.f );
		SetParamElementInfo( PID_SHAPE, 1, "Circle", 1.f );
		SetRangedParamInfo( PID_RADIUS, "Radius", FF_TYPE_STANDARD, 0.9f, 0.05f, 1.4f );
		SetRangedParamInfo( PID_SOFTNESS, "Softness", FF_TYPE_STANDARD, 0.1f, 0.001f, 1.f );
		SetParamInfo( PID_EDGE_ONLY, "Edge Only", FF_TYPE_BOOLEAN, false );

		SetParamInfo( PID_RED, "Red", FF_TYPE_RED, 1.f );
		SetParamInfo( PID_GREEN, "Green", FF_TYPE_GREEN, 1.f );
		SetParamInfo( PID_BLUE, "Blue", FF_TYPE_BLUE, 1.f );

		SetRangedParamInfo( PID_GRID_SIZE, "Grid Size", FF_TYPE_STANDARD, 0.f, 0.f, 400.f );
		SetParamInfo( PID_GRID_RED, "Grid Red", FF_TYPE_RED, 0.2f );
		SetParamInfo( PID_GRID_GREEN, "Grid Green", FF_TYPE_GREEN, 0.2f );
		SetParamInfo( PID_GRID_BLUE, "Grid Blue", FF_TYPE_BLUE, 0.2f );
		SetParamInfo( PID_SHOW_ORIGINAL, "Show Original", FF_TYPE_BOOLEAN, false );

		for( int id = PID_PHASE; id <= PID_DIRECTION; ++id )
			SetParamGroup( id, "Pattern" );
		for( int id = PID_SHAPE; id <= PID_BLUE; ++id )
			SetParamGroup( id, "Shape" );
		for( int id = PID_GRID_SIZE; id <= PID_GRID_BLUE; ++id )
			SetParamGroup( id, "Grid" );

		values[ PID_VIEW ]       = 0.f;
		values[ PID_TILE_SIZE ]  = 24.f;
		values[ PID_MODE ]       = (float)MODE_MINIMAL;
		values[ PID_PHASE ]      = 0.f;
		values[ PID_LENGTH ]     = 0.35f;
		values[ PID_SEED ]       = 0.f;
		values[ PID_RANDOMISE ]  = 0.f;
		values[ PID_FAN ]        = 0.f;
		values[ PID_DIRECTION ]  = (float)DIR_RIGHT;
		values[ PID_SHAPE ]      = 0.f;
		values[ PID_RADIUS ]     = 0.9f;
		values[ PID_SOFTNESS ]   = 0.1f;
		values[ PID_EDGE_ONLY ]  = 0.f;
		values[ PID_RED ]        = values[ PID_GREEN ] = values[ PID_BLUE ] = 1.f;
		values[ PID_GRID_SIZE ]  = 0.f;
		values[ PID_GRID_RED ]   = values[ PID_GRID_GREEN ] = values[ PID_GRID_BLUE ] = 0.2f;
		values[ PID_SHOW_ORIGINAL ] = 0.f;
		doc                         = DocumentWatcher::Instance().Current();
	}

	FFResult InitGL( const FFGLViewportStruct* vp ) override
	{
		const std::string vertex = std::string( CompositionVertexHeader() ) + vertexShaderBody;
		if( !shader.Compile( vertex.c_str(), fragmentShaderBody ) ||
			!copyShader.Compile( copyVertexShader, copyFragmentShader ) ||
			!quad.Initialise() ||
			!mesh.Init( { 2, 2, 2 } ) )
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
		if( index == PID_MODE || index == PID_EDGE_ONLY )
			UpdateVisibility( true );
		return FF_SUCCESS;
	}

	float GetFloatParameter( unsigned int index ) override
	{
		return index < PID_COUNT ? values[ index ] : 0.f;
	}

	char* GetParameterDisplay( unsigned int index ) override
	{
		if( index == PID_TILE_SIZE || index == PID_GRID_SIZE )
		{
			const float size = values[ index ];
			if( index == PID_GRID_SIZE && size < 1.f )
				std::snprintf( display, sizeof( display ), "off" );
			else
				std::snprintf( display, sizeof( display ), "%.0f px", size );
			return display;
		}
		return StilletchaserPlugin::GetParameterDisplay( index );
	}

	const char* GetShortName() override
	{
		static const char* sname = "STTiler";
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

		BuildVertices( *doc, input );
		if( vertices.empty() )
			return FF_SUCCESS;
		mesh.Upload( vertices );

		const View& view = doc->views[ Option( values[ PID_VIEW ], (int)doc->views.size() ) ];
		// Fan spreads the pattern's start across the slices that are actually shown.
		std::vector< int > shown;
		for( size_t s = 0; s < doc->slices.size(); ++s )
			if( s >= view.includes.size() || view.includes[ s ] )
				shown.push_back( (int)s );
		if( shown.empty() )
			return FF_SUCCESS;

		glEnable( GL_BLEND );
		glBlendFunc( GL_ONE, GL_ONE_MINUS_SRC_ALPHA );
		ScopedShaderBinding binding( shader.GetGLID() );
		ScopedSamplerActivation activateSampler( 0 );
		Scoped2DTextureBinding textureBinding( input.Handle );
		shader.Set( "inputTexture", 0 );
		shader.Set( "maxUV", maxCoords.s, maxCoords.t );
		shader.Set( "tileSize", std::clamp( values[ PID_TILE_SIZE ], 2.f, 400.f ) );
		shader.Set( "mode", Option( values[ PID_MODE ], 4 ) );
		shader.Set( "phase", values[ PID_PHASE ] );
		shader.Set( "runLength", std::clamp( values[ PID_LENGTH ], 0.01f, 1.f ) );
		shader.Set( "seed", values[ PID_SEED ] );
		shader.Set( "randomise", std::clamp( values[ PID_RANDOMISE ], 0.f, 1.f ) );
		shader.Set( "direction", Option( values[ PID_DIRECTION ], 4 ) );
		shader.Set( "shape", Option( values[ PID_SHAPE ], 2 ) );
		shader.Set( "radius", std::clamp( values[ PID_RADIUS ], 0.05f, 1.4f ) );
		shader.Set( "softness", std::clamp( values[ PID_SOFTNESS ], 0.001f, 1.f ) );
		shader.Set( "edgeOnly", values[ PID_EDGE_ONLY ] > 0.5f ? 1 : 0 );
		shader.Set( "colour", values[ PID_RED ], values[ PID_GREEN ], values[ PID_BLUE ] );
		shader.Set( "gridSize", values[ PID_GRID_SIZE ] );
		shader.Set( "gridColour", values[ PID_GRID_RED ], values[ PID_GRID_GREEN ], values[ PID_GRID_BLUE ] );

		const float fan = values[ PID_FAN ];
		glBindVertexArray( mesh.VAO() );
		for( size_t i = 0; i < shown.size(); ++i )
		{
			const int s = shown[ i ];
			shader.Set( "sliceSize", draws[ s ].size.x, draws[ s ].size.y );
			shader.Set( "uvOrigin", draws[ s ].uvOrigin.x, draws[ s ].uvOrigin.y );
			shader.Set( "uvAxisX", draws[ s ].uvAxisX.x, draws[ s ].uvAxisX.y );
			shader.Set( "uvAxisY", draws[ s ].uvAxisY.x, draws[ s ].uvAxisY.y );
			shader.Set( "sliceFan", fan * (float)i / (float)shown.size() );
			glDrawArrays( GL_TRIANGLES, draws[ s ].first, draws[ s ].count );
		}
		glBindVertexArray( 0 );
		glDisable( GL_BLEND );
		return FF_SUCCESS;
	}

private:
	struct SliceDraw
	{
		GLint first;
		GLsizei count;
		Vec2 size;// Composition pixels along the slice's own edges.
		Vec2 uvOrigin, uvAxisX, uvAxisY;
	};

	void BuildVertices( const ChaseDocument& d, const FFGLTextureStruct& input )
	{
		vertices.clear();
		draws.clear();
		const float contentAspect = input.Height ? (float)input.Width / (float)input.Height : 1.f;
		for( const Slice& slice : d.slices )
		{
			SliceDraw draw{ (GLint)( vertices.size() / FLOATS_PER_VERTEX ), (GLsizei)slice.indices.size(), SliceSize( d, slice ) };
			// Slice-local to texture coordinates is affine, so three corners describe it exactly.
			const Vec2 o  = ContentUV( d, slice, slice.origin, SCALING_MASK, contentAspect );
			const Vec2 ax = ContentUV( d, slice, { slice.origin.x + slice.axisX.x, slice.origin.y + slice.axisX.y }, SCALING_MASK, contentAspect );
			const Vec2 ay = ContentUV( d, slice, { slice.origin.x + slice.axisY.x, slice.origin.y + slice.axisY.y }, SCALING_MASK, contentAspect );
			draw.uvOrigin = o;
			draw.uvAxisX  = { ax.x - o.x, ax.y - o.y };
			draw.uvAxisY  = { ay.x - o.x, ay.y - o.y };
			draws.push_back( draw );
			for( uint16_t i : slice.indices )
			{
				const Vec2 p     = slice.points[ i ];
				const Vec2 uv    = ContentUV( d, slice, p, SCALING_MASK, contentAspect );
				const Vec2 local = SliceLocal( slice, p );
				vertices.insert( vertices.end(), { p.x, p.y, uv.x, uv.y, local.x, local.y } );
			}
		}
	}

	void RefreshMenus( bool raiseEvents )
	{
		const auto views = ViewNames( *doc );
		SetParamElements( PID_VIEW, views, Indices( views.size() ), raiseEvents );
		UpdateVisibility( raiseEvents );
	}

	// Texture takes its colour from the input, so the colour pickers do nothing there.
	void UpdateVisibility( bool raiseEvents )
	{
		const bool tinted = Option( values[ PID_MODE ], 4 ) != MODE_TEXTURE;
		for( int id = PID_RED; id <= PID_BLUE; ++id )
			SetParamVisibility( id, tinted, raiseEvents );
	}

	FFGLShader shader, copyShader;
	FFGLScreenQuad quad;
	FloatVertexBuffer mesh;
	std::vector< float > vertices;
	std::vector< SliceDraw > draws;
	std::shared_ptr< const ChaseDocument > doc;
	uint64_t docVersion = 0;
	char display[ 16 ]  = {};
	float values[ PID_COUNT ];
};

static CFFGLPluginInfo PluginInfo(
	PluginFactory< Tiler >,
	"STTI",       // Plugin unique ID
	"STLT-TILER", // Plugin name
	2,            // API major version number
	1,            // API minor version number
	1,            // Plugin major version number
	0,            // Plugin minor version number
	FF_EFFECT,
	"Animated tile patterns drawn inside Advanced Output slices",
	"Stilletchaser" );
