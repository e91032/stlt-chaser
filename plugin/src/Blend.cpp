// STLT-BLEND: a blend mode rather than an effect. The chase does not decide how opaque each slice
// is, it decides how far each slice has crossed from the layer below to the layer above. Put any
// effect on the top layer and the chase runs that effect through the slices: a chase that turns
// slices red and back, or sharpens them, or anything else, instead of only fading them in.
// Driver covers all three ways of playing it, so this one plugin stands in for Chaser Blend,
// Bumper Blend and Random Blend.
#include <FFGLSDK.h>
#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>
#include "ChaseDocument.h"
#include "ChaseDriver.h"
#include "PluginCommon.h"
using namespace ffglex;
using namespace stilletchaser;

namespace
{
const int SEQUENCE_BUTTONS = 32;
const int BUMP_BUTTONS     = 32;

enum ParamID
{
	PID_DRIVER,
	PID_SEQUENCE,
	PID_STEP,
	PID_VIEW,
	PID_ECHOES,
	PID_ENVELOPE,
	PID_SUSTAIN,
	PID_RELEASE,
	PID_AMOUNT,
	PID_INVERT,
	PID_SEQUENCE_BUTTON,

	PID_BUMP_BUTTON = PID_SEQUENCE_BUTTON + SEQUENCE_BUTTONS,
	PID_BUMP_HOLD   = PID_BUMP_BUTTON + BUMP_BUTTONS,
	PID_BUMP_LENGTH,
	PID_BUMP_SYNC,
	PID_BUMP_PREV,
	PID_BUMP_NEXT,
	PID_BUMP_BLAST,
	PID_BLAST_SPEED,
	PID_BUMP_AUTO_RESET,
	PID_BUMP_RESET_DELAY,
	PID_RANDOM_MAX,
	PID_RANDOM_SPEED,
	PID_COUNT
};

const int FLOATS_PER_VERTEX = 2;// compPos.xy only: both textures are read in screen space.

const char vertexShaderBody[] = R"(
layout( location = 0 ) in vec2 compPos;
out vec2 screenUV;
void main()
{
	gl_Position = compToClip( compPos );
	screenUV    = compPos;
}
)";

// Wherever a slice is lit, the layer above shows through by that much. Everywhere else the
// layer below is left exactly as it was, which is what makes this usable as a blend mode.
const char fragmentShaderBody[] = R"(#version 410 core
uniform sampler2D wetTexture;
uniform vec2 wetMaxUV;
uniform float level;
in vec2 screenUV;
out vec4 fragColor;
void main()
{
	if( level <= 0.0 )
		discard;
	vec2 uv   = vec2( screenUV.x, 1.0 - screenUV.y ) * wetMaxUV;
	fragColor = texture( wetTexture, uv ) * level;
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

class Blend : public StilletchaserPlugin
{
public:
	Blend()
	{
		SetMinInputs( 2 );
		SetMaxInputs( 2 );

		SetOptionParamInfo( PID_DRIVER, "Driver", DRIVERS, chase.driver );
		SetParamElementInfo( PID_DRIVER, 0, "Chaser", (float)DRIVER_CHASER );
		SetParamElementInfo( PID_DRIVER, 1, "Bumper", (float)DRIVER_BUMPER );
		SetParamElementInfo( PID_DRIVER, 2, "Random", (float)DRIVER_RANDOM );
		SetParamElementInfo( PID_DRIVER, 3, "None (all slices)", (float)DRIVER_NONE );

		SetOptionParamInfo( PID_SEQUENCE, "Sequence", 1, 0.f );
		SetParamElementInfo( PID_SEQUENCE, 0, "-", 0.f );
		SetParamInfo( PID_STEP, "Step", FF_TYPE_STANDARD, chase.step );
		SetOptionParamInfo( PID_VIEW, "View", 1, 0.f );
		SetParamElementInfo( PID_VIEW, 0, "All", 0.f );
		SetRangedParamInfo( PID_ECHOES, "Echoes", FF_TYPE_INTEGER, chase.echoes, 0.f, (float)MAX_ECHOES );
		SetOptionParamInfo( PID_ENVELOPE, "Envelope", 1, 0.f );
		SetParamElementInfo( PID_ENVELOPE, 0, "Sustain / Release", 0.f );
		SetParamInfo( PID_SUSTAIN, "Sustain", FF_TYPE_STANDARD, chase.sustain );
		SetParamInfo( PID_RELEASE, "Release", FF_TYPE_STANDARD, chase.release );
		SetParamInfo( PID_AMOUNT, "Amount", FF_TYPE_STANDARD, 1.f );
		SetParamInfo( PID_INVERT, "Invert", FF_TYPE_BOOLEAN, false );

		for( int i = 0; i < SEQUENCE_BUTTONS; ++i )
		{
			const std::string name = "Sequence " + std::to_string( i + 1 );
			SetParamInfo( PID_SEQUENCE_BUTTON + i, name.c_str(), FF_TYPE_EVENT, false );
			SetParamGroup( PID_SEQUENCE_BUTTON + i, "Sequences" );
		}
		for( int i = 0; i < BUMP_BUTTONS; ++i )
		{
			const std::string name = "Bump " + std::to_string( i + 1 );
			SetParamInfo( PID_BUMP_BUTTON + i, name.c_str(), FF_TYPE_EVENT, false );
			SetParamGroup( PID_BUMP_BUTTON + i, "Bumper" );
		}
		SetParamInfo( PID_BUMP_HOLD, "Bump Hold", FF_TYPE_BOOLEAN, false );
		SetRangedParamInfo( PID_BUMP_LENGTH, "Bump Length", FF_TYPE_STANDARD, chase.bumpLength, 0.02f, 5.f );
		SetParamInfo( PID_BUMP_SYNC, "Bump Sync", FF_TYPE_BOOLEAN, false );
		SetParamInfo( PID_BUMP_PREV, "Previous", FF_TYPE_EVENT, false );
		SetParamInfo( PID_BUMP_NEXT, "Next", FF_TYPE_EVENT, false );
		SetParamInfo( PID_BUMP_BLAST, "Blast", FF_TYPE_EVENT, false );
		SetRangedParamInfo( PID_BLAST_SPEED, "Blast Speed", FF_TYPE_STANDARD, chase.blastSpeed, 1.f, 50.f );
		SetParamInfo( PID_BUMP_AUTO_RESET, "Bump Auto Reset", FF_TYPE_BOOLEAN, true );
		SetRangedParamInfo( PID_BUMP_RESET_DELAY, "Bump Reset Delay", FF_TYPE_STANDARD, chase.bumpResetDelay, 0.1f, 10.f );
		for( unsigned int id = PID_BUMP_HOLD; id <= PID_BUMP_RESET_DELAY; ++id )
			SetParamGroup( id, "Bumper" );

		SetRangedParamInfo( PID_RANDOM_MAX, "Max Slices", FF_TYPE_STANDARD, chase.randomMax, 1.f, 16.f );
		SetRangedParamInfo( PID_RANDOM_SPEED, "Random Speed", FF_TYPE_STANDARD, chase.randomSpeed, 0.1f, 30.f );
		SetParamGroup( PID_RANDOM_MAX, "Random" );
		SetParamGroup( PID_RANDOM_SPEED, "Random" );

		doc = DocumentWatcher::Instance().Current();
	}

	FFResult InitGL( const FFGLViewportStruct* vp ) override
	{
		const std::string vertex = std::string( CompositionVertexHeader() ) + vertexShaderBody;
		if( !shader.Compile( vertex.c_str(), fragmentShaderBody ) ||
			!copyShader.Compile( copyVertexShader, copyFragmentShader ) ||
			!quad.Initialise() ||
			!mesh.Init( { 2 } ) )
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
		switch( index )
		{
		case PID_DRIVER:
			chase.driver = value;
			chase.Reset();
			UpdateVisibility( true );
			break;
		case PID_SEQUENCE:
			chase.sequence = value;
			UpdateSequenceButtons( true );
			break;
		case PID_STEP:     chase.step = value; break;
		case PID_VIEW:     chase.view = value; break;
		case PID_ECHOES:   chase.echoes = value; break;
		case PID_ENVELOPE:
			chase.envelope = value;
			UpdateVisibility( true );
			break;
		case PID_SUSTAIN:  chase.sustain = value; break;
		case PID_RELEASE:  chase.release = value; break;
		case PID_AMOUNT:   amount = value; break;
		case PID_INVERT:   invert = value; break;
		case PID_BUMP_HOLD:
			chase.bumpHold = value;
			UpdateVisibility( true );
			break;
		case PID_BUMP_LENGTH: chase.bumpLength = value; break;
		case PID_BUMP_SYNC:   chase.bumpSync = value; break;
		case PID_BLAST_SPEED: chase.blastSpeed = value; break;
		case PID_BUMP_AUTO_RESET:
			chase.bumpAutoReset = value;
			UpdateVisibility( true );
			break;
		case PID_BUMP_RESET_DELAY: chase.bumpResetDelay = value; break;
		case PID_RANDOM_MAX:       chase.randomMax = value; break;
		case PID_RANDOM_SPEED:     chase.randomSpeed = value; break;
		case PID_BUMP_PREV:
			if( value > 0.5f )
				chase.Tap( -1 );
			break;
		case PID_BUMP_NEXT:
			if( value > 0.5f )
				chase.Tap( 1 );
			break;
		case PID_BUMP_BLAST:
			chase.SetBlast( value > 0.5f ? !chase.Blasting() : false );
			break;
		default:
			if( index >= PID_SEQUENCE_BUTTON && index < PID_SEQUENCE_BUTTON + SEQUENCE_BUTTONS )
			{
				const int chosen = (int)( index - PID_SEQUENCE_BUTTON );
				if( value > 0.5f && chosen < (int)doc->sequences.size() )
				{
					chase.sequence = (float)chosen;
					RaiseParamEvent( PID_SEQUENCE, FF_EVENT_FLAG_VALUE );
					UpdateSequenceButtons( true );
					chase.Reset();
					UpdateBumpButtons( true );
				}
			}
			else if( index >= PID_BUMP_BUTTON && index < PID_BUMP_BUTTON + BUMP_BUTTONS )
			{
				const int chosen = (int)( index - PID_BUMP_BUTTON );
				if( value > 0.5f )
					chase.StartBump( chosen );
				else
					chase.EndBump( chosen );
			}
			else
				return FF_FAIL;
		}
		return FF_SUCCESS;
	}

	float GetFloatParameter( unsigned int index ) override
	{
		switch( index )
		{
		case PID_DRIVER:           return chase.driver;
		case PID_SEQUENCE:         return chase.sequence;
		case PID_STEP:             return chase.step;
		case PID_VIEW:             return chase.view;
		case PID_ECHOES:           return chase.echoes;
		case PID_ENVELOPE:         return chase.envelope;
		case PID_SUSTAIN:          return chase.sustain;
		case PID_RELEASE:          return chase.release;
		case PID_AMOUNT:           return amount;
		case PID_INVERT:           return invert;
		case PID_BUMP_HOLD:        return chase.bumpHold;
		case PID_BUMP_LENGTH:      return chase.bumpLength;
		case PID_BUMP_SYNC:        return chase.bumpSync;
		case PID_BLAST_SPEED:      return chase.blastSpeed;
		case PID_BUMP_AUTO_RESET:  return chase.bumpAutoReset;
		case PID_BUMP_RESET_DELAY: return chase.bumpResetDelay;
		case PID_RANDOM_MAX:       return chase.randomMax;
		case PID_RANDOM_SPEED:     return chase.randomSpeed;
		default:                   return 0.f;
		}
	}

	const char* GetShortName() override
	{
		static const char* sname = "STBlend";
		return sname;
	}

	FFResult ProcessOpenGL( ProcessOpenGLStruct* pGL ) override
	{
		if( pGL->numInputTextures < 2 || pGL->inputTextures[ 0 ] == nullptr || pGL->inputTextures[ 1 ] == nullptr )
			return FF_FAIL;
		// As a mixer, input 0 is the layer below and input 1 the layer above.
		const FFGLTextureStruct& dry = *pGL->inputTextures[ 0 ];
		const FFGLTextureStruct& wet = *pGL->inputTextures[ 1 ];
		DocumentWatcher& watcher     = DocumentWatcher::Instance();
		doc                          = watcher.Current();
		if( watcher.Version() != docVersion )
		{
			docVersion = watcher.Version();
			RefreshMenus( true );
		}

		// The layer below goes down whole; the chase then paints the layer above over it.
		{
			ScopedShaderBinding copyBinding( copyShader.GetGLID() );
			ScopedSamplerActivation activateSampler( 0 );
			Scoped2DTextureBinding textureBinding( dry.Handle );
			const FFGLTexCoords dryMax = GetMaxGLTexCoords( dry );
			copyShader.Set( "inputTexture", 0 );
			copyShader.Set( "maxUV", dryMax.s, dryMax.t );
			quad.Draw();
		}
		if( doc->slices.empty() )
			return FF_SUCCESS;

		chase.bpm = bpm;
		chase.Levels( *doc, levels );
		if( chase.TakePadsDirty() )
			UpdateBumpButtons( true );

		BuildVertices( *doc );
		if( vertices.empty() )
			return FF_SUCCESS;
		mesh.Upload( vertices );

		const FFGLTexCoords wetMax = GetMaxGLTexCoords( wet );
		glEnable( GL_BLEND );
		glBlendFunc( GL_ONE, GL_ONE_MINUS_SRC_ALPHA );
		ScopedShaderBinding binding( shader.GetGLID() );
		ScopedSamplerActivation activateSampler( 0 );
		Scoped2DTextureBinding textureBinding( wet.Handle );
		shader.Set( "wetTexture", 0 );
		shader.Set( "wetMaxUV", wetMax.s, wetMax.t );

		const float gain     = std::clamp( amount, 0.f, 1.f );
		const bool inverted  = invert > 0.5f;
		glBindVertexArray( mesh.VAO() );
		for( size_t s = 0; s < draws.size() && s < levels.size(); ++s )
		{
			float level = std::clamp( levels[ s ], 0.f, 1.f );
			if( inverted )
				level = 1.f - level;
			level *= gain;
			if( level <= 0.f )
				continue;
			shader.Set( "level", level );
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
	};

	void BuildVertices( const ChaseDocument& d )
	{
		vertices.clear();
		draws.clear();
		for( const Slice& slice : d.slices )
		{
			draws.push_back( { (GLint)( vertices.size() / FLOATS_PER_VERTEX ), (GLsizei)slice.indices.size() } );
			for( uint16_t i : slice.indices )
			{
				const Vec2 p = slice.points[ i ];
				vertices.insert( vertices.end(), { p.x, p.y } );
			}
		}
	}

	void RefreshMenus( bool raiseEvents )
	{
		const auto sequences = SequenceNames( *doc );
		const auto views     = ViewNames( *doc );
		const auto envelopes = EnvelopeNames( *doc, "Sustain / Release" );
		SetParamElements( PID_SEQUENCE, sequences, Indices( sequences.size() ), raiseEvents );
		SetParamElements( PID_VIEW, views, Indices( views.size() ), raiseEvents );
		SetParamElements( PID_ENVELOPE, envelopes, Indices( envelopes.size() ), raiseEvents );
		UpdateVisibility( raiseEvents );
	}

	void UpdateVisibility( bool raiseEvents )
	{
		const int which   = Option( chase.driver, DRIVERS );
		const bool chaser = which == DRIVER_CHASER;
		const bool bumper = which == DRIVER_BUMPER;
		const bool random = which == DRIVER_RANDOM;
		const bool played = chaser || bumper || random;
		const bool curve  = std::lround( chase.envelope ) > 0;

		SetParamVisibility( PID_STEP, chaser, raiseEvents );
		for( unsigned int id : { PID_ECHOES, PID_ENVELOPE } )
			SetParamVisibility( id, played, raiseEvents );
		SetParamVisibility( PID_SEQUENCE, ( chaser || bumper ) && doc->sequences.empty(), raiseEvents );
		const bool holding = bumper && chase.bumpHold > 0.5f;
		SetParamVisibility( PID_SUSTAIN, ( played && !curve ) || holding, raiseEvents );
		SetParamVisibility( PID_RELEASE, played && !curve, raiseEvents );
		for( unsigned int id : { PID_RANDOM_MAX, PID_RANDOM_SPEED } )
			SetParamVisibility( id, random, raiseEvents );
		for( unsigned int id = PID_BUMP_HOLD; id <= PID_BUMP_RESET_DELAY; ++id )
			SetParamVisibility( id, bumper, raiseEvents );
		SetParamVisibility( PID_BUMP_RESET_DELAY, bumper && chase.bumpAutoReset > 0.5f, raiseEvents );

		UpdateSequenceButtons( raiseEvents );
		UpdateBumpButtons( raiseEvents );
	}

	void UpdateSequenceButtons( bool raiseEvents )
	{
		const int which   = Option( chase.driver, DRIVERS );
		const bool shown  = which == DRIVER_CHASER || which == DRIVER_BUMPER;
		const int count   = (int)doc->sequences.size();
		const int chosen  = Option( chase.sequence, count );
		for( int i = 0; i < SEQUENCE_BUTTONS; ++i )
		{
			SetParamVisibility( PID_SEQUENCE_BUTTON + i, shown && i < count, raiseEvents );
			if( i < count )
			{
				std::string name = doc->sequences[ i ].name;
				if( i == chosen )
					name = "\xe2\x96\xb6 " + name;
				SetParamDisplayName( PID_SEQUENCE_BUTTON + i, name, raiseEvents );
			}
		}
	}

	void UpdateBumpButtons( bool raiseEvents )
	{
		const bool bumper = Option( chase.driver, DRIVERS ) == DRIVER_BUMPER;
		const int count   = bumper ? std::min( chase.StepCount( *doc ), BUMP_BUTTONS ) : 0;
		const std::vector< bool > sounding = chase.Sounding( BUMP_BUTTONS );
		for( int i = 0; i < BUMP_BUTTONS; ++i )
		{
			SetParamVisibility( PID_BUMP_BUTTON + i, i < count, raiseEvents );
			std::string name = "Bump " + std::to_string( i + 1 );
			if( sounding[ i ] )
				name = "\xe2\x96\xb6 " + name;
			SetParamDisplayName( PID_BUMP_BUTTON + i, name, raiseEvents );
		}
	}

	FFGLShader shader, copyShader;
	FFGLScreenQuad quad;
	FloatVertexBuffer mesh;
	std::vector< float > vertices;
	std::vector< SliceDraw > draws;
	std::vector< float > levels;
	ChaseDriver chase;
	float amount = 1.f;
	float invert = 0.f;
	std::shared_ptr< const ChaseDocument > doc;
	uint64_t docVersion = 0;
};

static CFFGLPluginInfo PluginInfo(
	PluginFactory< Blend >,
	"STBL",       // Plugin unique ID
	"STLT-BLEND", // Plugin name
	2,            // API major version number
	1,            // API minor version number
	1,            // Plugin major version number
	0,            // Plugin minor version number
	FF_MIXER,
	"Chases between two layers, so any effect can be chased through the slices",
	"Stilletchaser" );
