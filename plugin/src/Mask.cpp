// STLT-MASK: a blend mode that shows the layer below through the brightness of the layer above.
// Made for STLT-OUTLINER and STLT-TILER sitting on the top layer: their outlines or tiles become
// a stencil, so the footage underneath shows only there, instead of them drawing over it.
#include <FFGLSDK.h>
#include <algorithm>
using namespace ffglex;

namespace
{
enum ParamID
{
	PID_SOURCE,
	PID_INVERT,
	PID_AMOUNT,
	PID_COUNT
};

enum Source
{
	SOURCE_LUMINANCE,
	SOURCE_ALPHA,
	SOURCE_MAX,
};

const char vertexShader[] = R"(#version 410 core
layout( location = 0 ) in vec4 vPosition;
layout( location = 1 ) in vec2 vUV;
out vec2 uv;
void main()
{
	gl_Position = vPosition;
	uv          = vUV;
}
)";

const char fragmentShader[] = R"(#version 410 core
uniform sampler2D dryTexture;
uniform sampler2D wetTexture;
uniform vec2 dryMaxUV;
uniform vec2 wetMaxUV;
uniform int source;
uniform int invert;
uniform float amount;
in vec2 uv;
out vec4 fragColor;
void main()
{
	vec4 dry = texture( dryTexture, uv * dryMaxUV );
	vec4 wet = texture( wetTexture, uv * wetMaxUV );

	// The textures arrive premultiplied, so undo that before reading brightness: otherwise a
	// transparent bright pixel would read as dark and the mask would have soft edges twice over.
	vec3 straight = wet.a > 0.0 ? wet.rgb / wet.a : wet.rgb;
	float mask;
	if( source == 0 )
		mask = dot( straight, vec3( 0.2126, 0.7152, 0.0722 ) ) * wet.a;
	else if( source == 1 )
		mask = wet.a;
	else
		mask = max( max( straight.r, straight.g ), straight.b ) * wet.a;

	if( invert != 0 )
		mask = 1.0 - mask;
	mask      = clamp( mix( 1.0, mask, clamp( amount, 0.0, 1.0 ) ), 0.0, 1.0 );
	fragColor = dry * mask;
}
)";
}// namespace

class Mask : public CFFGLPlugin
{
public:
	Mask()
	{
		SetMinInputs( 2 );
		SetMaxInputs( 2 );
		SetOptionParamInfo( PID_SOURCE, "Mask From", 3, (float)SOURCE_LUMINANCE );
		SetParamElementInfo( PID_SOURCE, SOURCE_LUMINANCE, "Luminance", (float)SOURCE_LUMINANCE );
		SetParamElementInfo( PID_SOURCE, SOURCE_ALPHA, "Alpha", (float)SOURCE_ALPHA );
		SetParamElementInfo( PID_SOURCE, SOURCE_MAX, "Brightest Channel", (float)SOURCE_MAX );
		SetParamInfo( PID_INVERT, "Invert", FF_TYPE_BOOLEAN, false );
		SetParamInfo( PID_AMOUNT, "Amount", FF_TYPE_STANDARD, 1.f );
		values[ PID_SOURCE ] = (float)SOURCE_LUMINANCE;
		values[ PID_INVERT ] = 0.f;
		values[ PID_AMOUNT ] = 1.f;
	}

	FFResult InitGL( const FFGLViewportStruct* vp ) override
	{
		if( !shader.Compile( vertexShader, fragmentShader ) || !quad.Initialise() )
		{
			DeInitGL();
			return FF_FAIL;
		}
		return FF_SUCCESS;
	}

	FFResult DeInitGL() override
	{
		shader.FreeGLResources();
		quad.Release();
		return FF_SUCCESS;
	}

	FFResult SetFloatParameter( unsigned int index, float value ) override
	{
		if( index >= PID_COUNT )
			return FF_FAIL;
		values[ index ] = value;
		return FF_SUCCESS;
	}

	float GetFloatParameter( unsigned int index ) override
	{
		return index < PID_COUNT ? values[ index ] : 0.f;
	}

	const char* GetShortName() override
	{
		static const char* sname = "STMask";
		return sname;
	}

	FFResult ProcessOpenGL( ProcessOpenGLStruct* pGL ) override
	{
		if( pGL->numInputTextures < 2 || pGL->inputTextures[ 0 ] == nullptr || pGL->inputTextures[ 1 ] == nullptr )
			return FF_FAIL;
		const FFGLTextureStruct& dry = *pGL->inputTextures[ 0 ];
		const FFGLTextureStruct& wet = *pGL->inputTextures[ 1 ];
		const FFGLTexCoords dryMax   = GetMaxGLTexCoords( dry );
		const FFGLTexCoords wetMax   = GetMaxGLTexCoords( wet );

		ScopedShaderBinding binding( shader.GetGLID() );
		glActiveTexture( GL_TEXTURE0 );
		glBindTexture( GL_TEXTURE_2D, dry.Handle );
		glActiveTexture( GL_TEXTURE1 );
		glBindTexture( GL_TEXTURE_2D, wet.Handle );
		shader.Set( "dryTexture", 0 );
		shader.Set( "wetTexture", 1 );
		shader.Set( "dryMaxUV", dryMax.s, dryMax.t );
		shader.Set( "wetMaxUV", wetMax.s, wetMax.t );
		shader.Set( "source", (int)std::lround( values[ PID_SOURCE ] ) );
		shader.Set( "invert", values[ PID_INVERT ] > 0.5f ? 1 : 0 );
		shader.Set( "amount", values[ PID_AMOUNT ] );
		quad.Draw();

		glActiveTexture( GL_TEXTURE1 );
		glBindTexture( GL_TEXTURE_2D, 0 );
		glActiveTexture( GL_TEXTURE0 );
		glBindTexture( GL_TEXTURE_2D, 0 );
		return FF_SUCCESS;
	}

private:
	FFGLShader shader;
	FFGLScreenQuad quad;
	float values[ PID_COUNT ];
};

static CFFGLPluginInfo PluginInfo(
	PluginFactory< Mask >,
	"STMK",      // Plugin unique ID
	"STLT-MASK", // Plugin name
	2,           // API major version number
	1,           // API minor version number
	1,           // Plugin major version number
	0,           // Plugin minor version number
	FF_MIXER,
	"Shows the layer below through the brightness of the layer above",
	"Stilletchaser" );
