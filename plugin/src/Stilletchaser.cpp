#include "Stilletchaser.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
using namespace ffglex;
using namespace stilletchaser;

static CFFGLPluginInfo PluginInfo(
	PluginFactory< Stilletchaser >,
	"STCH",         // Plugin unique ID
	"STLT-CHASER",  // Plugin name
	2,              // API major version number
	1,              // API minor version number
	1,              // Plugin major version number
	2,              // Plugin minor version number
	FF_EFFECT,
	"Chases and delays Advanced Output slices with sequences made in the Stilletchaser editor",
	"Stilletchaser" );

// The two button banks at the end of the parameter list. New parameters go after them so the
// ids of everything above stay put and compositions saved with an earlier build still load.
static const int SEQUENCE_BUTTONS = 32;
static const int BUMP_BUTTONS     = 32;

enum ParamID
{
	PID_DRIVER,
	PID_SEQUENCE,
	PID_STEP,
	PID_SCALING,
	PID_VIEW,
	PID_ECHOES,
	PID_ENVELOPE,
	PID_SUSTAIN,
	PID_RELEASE,
	PID_DELAYER,
	PID_DELAY_SEQUENCE,
	PID_DELAY,
	PID_DELAY_UNIT,
	PID_MAX_FRAMES,
	PID_MAX_SECONDS,
	PID_MAX_BEATS,
	PID_QUALITY,
	PID_REVERSE,
	PID_AUTO_RESET,
	PID_RESET_MINUTES,
	// Wiper module, in the order of Stilletchaser::wipe[].
	PID_WIPER,
	PID_WIPE_SEQUENCE,
	PID_WIPE_STEP,
	PID_WIPE_LENGTH,
	PID_WIPE_OVERLAP,
	PID_WIPE_LOOPS,
	PID_WIPE_FLIP,
	PID_WIPE_ODD_EVEN,
	PID_WIPE_SWAP,
	PID_WIPE_ENVELOPE,
	PID_WIPE_SOURCE,
	PID_WIPE_RED,
	PID_WIPE_GREEN,
	PID_WIPE_BLUE,
	// Outliner module, in the order of Stilletchaser::outline[].
	PID_OUTLINER,
	PID_OUTLINE_TARGET,
	PID_OUTLINES_ONLY,
	PID_OUTLINE_RED,
	PID_OUTLINE_GREEN,
	PID_OUTLINE_BLUE,
	PID_OUTLINE_WIDTH,
	PID_OUTLINE_FEATHER,
	PID_OUTLINE_ANIMATE,
	PID_OUTLINE_PHASE,
	PID_OUTLINE_FAN,
	PID_OUTLINE_RANDOMISE,
	PID_OUTLINE_LENGTH,
	PID_OUTLINE_COUNT,
	PID_OUTLINE_FLIP,
	PID_OUTLINE_ODD_EVEN,
	PID_OUTLINE_ENVELOPE,

	// One button per sequence of the document (the first SEQUENCE_BUTTONS): pressing it, or its MIDI note, chooses
	// that sequence. Shown for the sequences there are, named after them; the one playing is marked.
	PID_SEQUENCE_BUTTON,

	// One button per step of the chosen sequence (the first BUMP_BUTTONS): pressing it, or its
	// MIDI note, fires that step. Then the Bumper's own controls.
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
};

enum OutlineTarget
{
	TARGET_ALL,
	TARGET_LIT,
};

enum WipeSource
{
	SOURCE_VIDEO,
	SOURCE_COLOUR,
};


enum DelayUnit
{
	UNIT_FRAMES,
	UNIT_SECONDS,
	UNIT_BEATS,
};

static const int MAX_HISTORY       = 1500;// Frames; about 25 s at 60 fps.
static const size_t MAX_HISTORY_VRAM = (size_t)3 << 30;
static const int FLOATS_PER_VERTEX = 6;// position xy, content uv, slice-local xy

static const char vertexShaderBody[] = R"(
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

// A slice is drawn either at a flat chase level or as a wipe band, showing the content or a
// solid colour. Draws are combined so a slice shows wherever the chase or a wipe lights it.
static const char fragmentShaderBody[] = R"(
uniform sampler2D inputTexture;
uniform vec2 maxUV;
uniform float level;
uniform int wipeMode;
uniform int useColour;
uniform vec3 colour;
in vec2 uv;
in vec2 local;
out vec4 fragColor;
void main()
{
	float a = wipeMode != 0 ? wipeAlpha( local ) : level;
	if( a <= 0.0 )
		discard;
	vec4 source = useColour != 0 ? vec4( colour, 1.0 ) : texture( inputTexture, uv * maxUV );
	fragColor   = source * a;
}
)";

Stilletchaser::Stilletchaser()
{
	SetMinInputs( 1 );
	SetMaxInputs( 1 );

	SetOptionParamInfo( PID_DRIVER, "Driver", DRIVERS, chase.driver );
	SetParamElementInfo( PID_DRIVER, 0, "Chaser", (float)DRIVER_CHASER );
	SetParamElementInfo( PID_DRIVER, 1, "Bumper", (float)DRIVER_BUMPER );
	SetParamElementInfo( PID_DRIVER, 2, "Random", (float)DRIVER_RANDOM );
	SetParamElementInfo( PID_DRIVER, 3, "None (all slices)", (float)DRIVER_NONE );

	SetOptionParamInfo( PID_SEQUENCE, "Sequence", 1, 0.f );
	SetParamElementInfo( PID_SEQUENCE, 0, "-", 0.f );
	SetParamInfo( PID_STEP, "Step", FF_TYPE_STANDARD, chase.step );

	SetOptionParamInfo( PID_SCALING, "Scaling", 3, scaling );
	SetParamElementInfo( PID_SCALING, SCALING_MASK, "Mask", (float)SCALING_MASK );
	SetParamElementInfo( PID_SCALING, SCALING_FILL, "Fill", (float)SCALING_FILL );
	SetParamElementInfo( PID_SCALING, SCALING_STRETCH, "Stretch", (float)SCALING_STRETCH );

	SetOptionParamInfo( PID_VIEW, "View", 1, 0.f );
	SetParamElementInfo( PID_VIEW, 0, "All", 0.f );

	SetRangedParamInfo( PID_ECHOES, "Echoes", FF_TYPE_INTEGER, chase.echoes, 0.f, (float)MAX_ECHOES );

	SetOptionParamInfo( PID_ENVELOPE, "Envelope", 1, 0.f );
	SetParamElementInfo( PID_ENVELOPE, 0, "Sustain / Release", 0.f );
	SetParamInfo( PID_SUSTAIN, "Sustain", FF_TYPE_STANDARD, chase.sustain );
	SetParamInfo( PID_RELEASE, "Release", FF_TYPE_STANDARD, chase.release );

	// Delayer module.
	SetParamInfo( PID_DELAYER, "Delayer", FF_TYPE_BOOLEAN, false );
	SetOptionParamInfo( PID_DELAY_SEQUENCE, "Delay Sequence", 1, 0.f );
	SetParamElementInfo( PID_DELAY_SEQUENCE, 0, "-", 0.f );
	SetParamInfo( PID_DELAY, "Delay", FF_TYPE_STANDARD, delay );
	SetOptionParamInfo( PID_DELAY_UNIT, "Set Delay In", 3, delayUnit );
	SetParamElementInfo( PID_DELAY_UNIT, UNIT_FRAMES, "Frames", (float)UNIT_FRAMES );
	SetParamElementInfo( PID_DELAY_UNIT, UNIT_SECONDS, "Seconds", (float)UNIT_SECONDS );
	SetParamElementInfo( PID_DELAY_UNIT, UNIT_BEATS, "Beats", (float)UNIT_BEATS );
	SetRangedParamInfo( PID_MAX_FRAMES, "Max Frames", FF_TYPE_INTEGER, maxFrames, 1.f, (float)MAX_HISTORY );
	SetRangedParamInfo( PID_MAX_SECONDS, "Max Seconds", FF_TYPE_STANDARD, maxSeconds, 0.05f, 25.f );
	SetRangedParamInfo( PID_MAX_BEATS, "Max Beats", FF_TYPE_STANDARD, maxBeats, 0.25f, 32.f );
	SetRangedParamInfo( PID_QUALITY, "Quality", FF_TYPE_STANDARD, quality, 0.1f, 1.f );
	SetParamInfo( PID_REVERSE, "Reverse", FF_TYPE_BOOLEAN, false );
	SetParamInfo( PID_AUTO_RESET, "Auto Reset", FF_TYPE_BOOLEAN, true );
	SetRangedParamInfo( PID_RESET_MINUTES, "Reset After (min)", FF_TYPE_INTEGER, resetMinutes, 1.f, 60.f );
	for( unsigned int id = PID_DELAYER; id <= PID_RESET_MINUTES; ++id )
		SetParamGroup( id, "Delayer" );

	// Wiper module: gradient wipes through the slices of its own sequence, on top of the 
	SetParamInfo( PID_WIPER, "Wiper", FF_TYPE_BOOLEAN, false );
	SetOptionParamInfo( PID_WIPE_SEQUENCE, "Wipe Sequence", 1, 0.f );
	SetParamElementInfo( PID_WIPE_SEQUENCE, 0, "-", 0.f );
	SetParamInfo( PID_WIPE_STEP, "Wipe Step", FF_TYPE_STANDARD, 0.f );
	SetRangedParamInfo( PID_WIPE_LENGTH, "Wipe Length", FF_TYPE_STANDARD, 0.25f, 0.01f, 1.f );
	SetRangedParamInfo( PID_WIPE_OVERLAP, "Wipe Overlap", FF_TYPE_STANDARD, 0.f, 0.f, 0.95f );
	SetRangedParamInfo( PID_WIPE_LOOPS, "Wipe Loops", FF_TYPE_INTEGER, 1.f, 1.f, (float)MAX_WIPE_LOOPS );
	SetParamInfo( PID_WIPE_FLIP, "Wipe Flip", FF_TYPE_BOOLEAN, false );
	SetParamInfo( PID_WIPE_ODD_EVEN, "Wipe Odd/Even", FF_TYPE_BOOLEAN, false );
	SetParamInfo( PID_WIPE_SWAP, "Wipe Swap", FF_TYPE_BOOLEAN, false );
	SetOptionParamInfo( PID_WIPE_ENVELOPE, "Wipe Envelope", 1, 0.f );
	SetParamElementInfo( PID_WIPE_ENVELOPE, 0, "Gradient", 0.f );
	SetOptionParamInfo( PID_WIPE_SOURCE, "Wipe Source", 2, (float)SOURCE_VIDEO );
	SetParamElementInfo( PID_WIPE_SOURCE, SOURCE_VIDEO, "Video", (float)SOURCE_VIDEO );
	SetParamElementInfo( PID_WIPE_SOURCE, SOURCE_COLOUR, "Solid Color", (float)SOURCE_COLOUR );
	SetParamInfo( PID_WIPE_RED, "Wipe Red", FF_TYPE_RED, 1.f );
	SetParamInfo( PID_WIPE_GREEN, "Wipe Green", FF_TYPE_GREEN, 1.f );
	SetParamInfo( PID_WIPE_BLUE, "Wipe Blue", FF_TYPE_BLUE, 1.f );
	for( unsigned int id = PID_WIPER; id <= PID_WIPE_BLUE; ++id )
		SetParamGroup( id, "Wiper" );

	// Outliner module: outlines over every slice of the view, or only over the lit ones.
	SetParamInfo( PID_OUTLINER, "Outliner", FF_TYPE_BOOLEAN, false );
	SetOptionParamInfo( PID_OUTLINE_TARGET, "Outline Target", 2, (float)TARGET_LIT );
	SetParamElementInfo( PID_OUTLINE_TARGET, TARGET_ALL, "All Slices", (float)TARGET_ALL );
	SetParamElementInfo( PID_OUTLINE_TARGET, TARGET_LIT, "Lit Slices", (float)TARGET_LIT );
	SetParamInfo( PID_OUTLINES_ONLY, "Outlines Only", FF_TYPE_BOOLEAN, false );
	SetParamInfo( PID_OUTLINE_RED, "Outline Red", FF_TYPE_RED, 1.f );
	SetParamInfo( PID_OUTLINE_GREEN, "Outline Green", FF_TYPE_GREEN, 1.f );
	SetParamInfo( PID_OUTLINE_BLUE, "Outline Blue", FF_TYPE_BLUE, 1.f );
	SetRangedParamInfo( PID_OUTLINE_WIDTH, "Outline Width", FF_TYPE_STANDARD, 10.f, 1.f, 300.f );
	SetRangedParamInfo( PID_OUTLINE_FEATHER, "Outline Feather", FF_TYPE_STANDARD, 0.f, 0.f, 500.f );
	SetParamInfo( PID_OUTLINE_ANIMATE, "Outline Animate", FF_TYPE_BOOLEAN, false );
	SetParamInfo( PID_OUTLINE_PHASE, "Outline Phase", FF_TYPE_STANDARD, 0.f );
	SetParamInfo( PID_OUTLINE_FAN, "Outline Fan", FF_TYPE_STANDARD, 0.f );
	SetParamInfo( PID_OUTLINE_RANDOMISE, "Outline Randomise", FF_TYPE_STANDARD, 0.f );
	SetRangedParamInfo( PID_OUTLINE_LENGTH, "Outline Length", FF_TYPE_STANDARD, 0.25f, 0.01f, 1.f );
	SetRangedParamInfo( PID_OUTLINE_COUNT, "Outline Count", FF_TYPE_INTEGER, 1.f, 1.f, (float)OutlineRenderer::MAX_LINES );
	SetParamInfo( PID_OUTLINE_FLIP, "Outline Flip", FF_TYPE_BOOLEAN, false );
	SetParamInfo( PID_OUTLINE_ODD_EVEN, "Outline Odd/Even", FF_TYPE_BOOLEAN, false );
	SetOptionParamInfo( PID_OUTLINE_ENVELOPE, "Outline Envelope", 1, 0.f );
	SetParamElementInfo( PID_OUTLINE_ENVELOPE, 0, "Taper", 0.f );
	for( unsigned int id = PID_OUTLINER; id <= PID_OUTLINE_ENVELOPE; ++id )
		SetParamGroup( id, "Outliner" );

	for( int i = 0; i < SEQUENCE_BUTTONS; ++i )
	{
		const std::string name = "Sequence " + std::to_string( i + 1 );
		SetParamInfo( PID_SEQUENCE_BUTTON + i, name.c_str(), FF_TYPE_EVENT, false );
		SetParamGroup( PID_SEQUENCE_BUTTON + i, "Sequences" );
	}

	// Bumper: a pad per step of the chosen sequence, plus how long a bump lasts.
	for( int i = 0; i < BUMP_BUTTONS; ++i )
	{
		const std::string name = "Bump " + std::to_string( i + 1 );
		SetParamInfo( PID_BUMP_BUTTON + i, name.c_str(), FF_TYPE_EVENT, false );
		SetParamGroup( PID_BUMP_BUTTON + i, "Bumper" );
	}
	SetParamInfo( PID_BUMP_HOLD, "Bump Hold", FF_TYPE_BOOLEAN, false );
	SetRangedParamInfo( PID_BUMP_LENGTH, "Bump Length", FF_TYPE_STANDARD, chase.bumpLength, 0.02f, 5.f );
	SetParamInfo( PID_BUMP_SYNC, "Bump Sync", FF_TYPE_BOOLEAN, false );
	// Tapping the sequence by hand, and the Blast that runs it on its own.
	SetParamInfo( PID_BUMP_PREV, "Previous", FF_TYPE_EVENT, false );
	SetParamInfo( PID_BUMP_NEXT, "Next", FF_TYPE_EVENT, false );
	SetParamInfo( PID_BUMP_BLAST, "Blast", FF_TYPE_EVENT, false );
	SetRangedParamInfo( PID_BLAST_SPEED, "Blast Speed", FF_TYPE_STANDARD, chase.blastSpeed, 1.f, 50.f );
	SetParamInfo( PID_BUMP_AUTO_RESET, "Bump Auto Reset", FF_TYPE_BOOLEAN, true );
	SetRangedParamInfo( PID_BUMP_RESET_DELAY, "Bump Reset Delay", FF_TYPE_STANDARD, chase.bumpResetDelay, 0.1f, 10.f );
	for( unsigned int id = PID_BUMP_HOLD; id <= PID_BUMP_RESET_DELAY; ++id )
		SetParamGroup( id, "Bumper" );

	// Random: how many slices a pick lights, and how often it picks.
	SetRangedParamInfo( PID_RANDOM_MAX, "Max Slices", FF_TYPE_STANDARD, chase.randomMax, 1.f, 16.f );
	SetRangedParamInfo( PID_RANDOM_SPEED, "Random Speed", FF_TYPE_STANDARD, chase.randomSpeed, 0.1f, 30.f );
	SetParamGroup( PID_RANDOM_MAX, "Random" );
	SetParamGroup( PID_RANDOM_SPEED, "Random" );

	doc        = DocumentWatcher::Instance().Current();
	docVersion = DocumentWatcher::Instance().Version();
	RefreshMenus( false );
	if( !doc->error.empty() )
		FFGLLog::LogToHost( ( "Stilletchaser: " + doc->error ).c_str() );
}

// Sequence, View and Envelope menus list what is in the editor's document.
void Stilletchaser::RefreshMenus( bool raiseEvents )
{
	const auto sequences = SequenceNames( *doc );
	const auto views     = ViewNames( *doc );
	const auto envelopes = EnvelopeNames( *doc, "Sustain / Release" );
	SetParamElements( PID_SEQUENCE, sequences, Indices( sequences.size() ), raiseEvents );
	SetParamElements( PID_DELAY_SEQUENCE, sequences, Indices( sequences.size() ), raiseEvents );
	SetParamElements( PID_WIPE_SEQUENCE, sequences, Indices( sequences.size() ), raiseEvents );
	SetParamElements( PID_VIEW, views, Indices( views.size() ), raiseEvents );
	SetParamElements( PID_ENVELOPE, envelopes, Indices( envelopes.size() ), raiseEvents );
	const auto wipeEnvelopes = EnvelopeNames( *doc, "Gradient" );
	SetParamElements( PID_WIPE_ENVELOPE, wipeEnvelopes, Indices( wipeEnvelopes.size() ), raiseEvents );
	const auto outlineEnvelopes = EnvelopeNames( *doc, "Taper" );
	SetParamElements( PID_OUTLINE_ENVELOPE, outlineEnvelopes, Indices( outlineEnvelopes.size() ), raiseEvents );
	UpdateVisibility( raiseEvents );
}

// Only the controls that do something with the current choices are shown.
void Stilletchaser::UpdateVisibility( bool raiseEvents )
{
	const bool chaser = Option( chase.driver, DRIVERS ) == DRIVER_CHASER;
	const bool bumper = Option( chase.driver, DRIVERS ) == DRIVER_BUMPER;
	const bool random = Option( chase.driver, DRIVERS ) == DRIVER_RANDOM;
	const bool played = chaser || bumper || random;
	const bool curve  = std::lround( chase.envelope ) > 0;
	// Step is the Chaser's own; a Bumper is stepped by its pads, a Random by itself.
	SetParamVisibility( PID_STEP, chaser, raiseEvents );
	for( unsigned int id : { PID_ECHOES, PID_ENVELOPE } )
		SetParamVisibility( id, played, raiseEvents );
	// Random picks slices straight out of the View, so it has no use for sequences.
	for( unsigned int id : { PID_RANDOM_MAX, PID_RANDOM_SPEED } )
		SetParamVisibility( id, random, raiseEvents );
	// The sequences are buttons; the menu only says there are none yet (it stays a parameter, for saved settings).
	SetParamVisibility( PID_SEQUENCE, ( chaser || bumper ) && doc->sequences.empty(), raiseEvents );
	UpdateSequenceButtons( raiseEvents );
	// Held bumps wait at Sustain, so it stays useful there even when a drawn curve is chosen.
	const bool holding = bumper && chase.bumpHold > 0.5f;
	SetParamVisibility( PID_SUSTAIN, ( played && !curve ) || holding, raiseEvents );
	SetParamVisibility( PID_RELEASE, played && !curve, raiseEvents );

	UpdateBumpButtons( raiseEvents );
	for( unsigned int id = PID_BUMP_HOLD; id <= PID_BUMP_RESET_DELAY; ++id )
		SetParamVisibility( id, bumper, raiseEvents );
	SetParamVisibility( PID_BUMP_RESET_DELAY, bumper && chase.bumpAutoReset > 0.5f, raiseEvents );

	const bool delaying = delayer > 0.5f;
	const int unit      = Option( delayUnit, 3 );
	for( unsigned int id = PID_DELAY_SEQUENCE; id <= PID_RESET_MINUTES; ++id )
		SetParamVisibility( id, delaying, raiseEvents );
	SetParamVisibility( PID_MAX_FRAMES, delaying && unit == UNIT_FRAMES, raiseEvents );
	SetParamVisibility( PID_MAX_SECONDS, delaying && unit == UNIT_SECONDS, raiseEvents );
	SetParamVisibility( PID_MAX_BEATS, delaying && unit == UNIT_BEATS, raiseEvents );
	SetParamVisibility( PID_RESET_MINUTES, delaying && autoReset > 0.5f, raiseEvents );

	const bool wiping = WipeParam( PID_WIPER ) > 0.5f;
	const bool colour = Option( WipeParam( PID_WIPE_SOURCE ), 2 ) == SOURCE_COLOUR;
	for( unsigned int id = PID_WIPE_SEQUENCE; id <= PID_WIPE_SOURCE; ++id )
		SetParamVisibility( id, wiping, raiseEvents );
	for( unsigned int id = PID_WIPE_RED; id <= PID_WIPE_BLUE; ++id )
		SetParamVisibility( id, wiping && colour, raiseEvents );

	// Feather is for static outlines; the animation controls only matter when animating.
	const bool outlining = OutlineParam( PID_OUTLINER ) > 0.5f;
	const bool animate   = OutlineParam( PID_OUTLINE_ANIMATE ) > 0.5f;
	for( unsigned int id = PID_OUTLINE_TARGET; id <= PID_OUTLINE_ANIMATE; ++id )
		SetParamVisibility( id, outlining, raiseEvents );
	SetParamVisibility( PID_OUTLINE_FEATHER, outlining && !animate, raiseEvents );
	for( unsigned int id = PID_OUTLINE_PHASE; id <= PID_OUTLINE_ENVELOPE; ++id )
		SetParamVisibility( id, outlining && animate, raiseEvents );
}

void Stilletchaser::UpdateSequenceButtons( bool raiseEvents )
{
	const int d        = Option( chase.driver, DRIVERS );
	const bool chaser  = d == DRIVER_CHASER || d == DRIVER_BUMPER;
	const int count    = (int)doc->sequences.size();
	const int selected = count > 0 ? Option( chase.sequence, count ) : -1;
	for( int i = 0; i < SEQUENCE_BUTTONS; ++i )
	{
		const bool shown = chaser && i < count;
		SetParamVisibility( PID_SEQUENCE_BUTTON + i, shown, raiseEvents );
		std::string name = i < count ? doc->sequences[ i ].name : "Sequence " + std::to_string( i + 1 );
		if( i == selected )
			name = "\xe2\x96\xb6 " + name; // the one playing
		SetParamDisplayName( PID_SEQUENCE_BUTTON + i, name, raiseEvents );
	}
}

// A pad per step of the chosen sequence, named after its number; the ones sounding are marked.
void Stilletchaser::UpdateBumpButtons( bool raiseEvents )
{
	const bool bumper = Option( chase.driver, DRIVERS ) == DRIVER_BUMPER;
	const int count = bumper ? std::min( chase.StepCount( *doc ), BUMP_BUTTONS ) : 0;

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

float Stilletchaser::WipeParam( unsigned int index ) const
{
	return wipe[ index - PID_WIPER ];
}

float Stilletchaser::OutlineParam( unsigned int index ) const
{
	return outline[ index - PID_OUTLINER ];
}

FFResult Stilletchaser::InitGL( const FFGLViewportStruct* vp )
{
	const std::string vertex   = std::string( CompositionVertexHeader() ) + vertexShaderBody;
	const std::string fragment = std::string( "#version 410 core\n" ) + WipeAlphaGLSL() + fragmentShaderBody;
	if( !shader.Compile( vertex, fragment ) || !history.InitGL() || !outlines.InitGL() )
	{
		DeInitGL();
		return FF_FAIL;
	}
	glGenVertexArrays( 1, &vao );
	glGenBuffers( 1, &vbo );
	glBindVertexArray( vao );
	glBindBuffer( GL_ARRAY_BUFFER, vbo );
	for( GLuint attribute = 0; attribute < 3; ++attribute )
	{
		glEnableVertexAttribArray( attribute );
		glVertexAttribPointer( attribute, 2, GL_FLOAT, GL_FALSE, FLOATS_PER_VERTEX * sizeof( float ), (void*)( attribute * 2 * sizeof( float ) ) );
	}
	glBindVertexArray( 0 );
	glBindBuffer( GL_ARRAY_BUFFER, 0 );
	return CFFGLPlugin::InitGL( vp );
}

FFResult Stilletchaser::DeInitGL()
{
	shader.FreeGLResources();
	history.DeInitGL();
	outlines.DeInitGL();
	if( vbo )
		glDeleteBuffers( 1, &vbo );
	if( vao )
		glDeleteVertexArrays( 1, &vao );
	vbo = vao = 0;
	return FF_SUCCESS;
}

bool Stilletchaser::DelayInFrames() const
{
	return Option( delayUnit, 3 ) == UNIT_FRAMES;
}

float Stilletchaser::MaxDelay() const
{
	switch( Option( delayUnit, 3 ) )
	{
	case UNIT_FRAMES:  return std::clamp( std::round( maxFrames ), 1.f, (float)MAX_HISTORY );
	case UNIT_SECONDS: return std::max( maxSeconds, 0.f );
	default:           return std::max( maxBeats, 0.f ) * 60.f / std::max( bpm, 1.f );
	}
}

// The delay sequence spreads the delay over the stage: slices in its first step are not
// delayed, slices in its last step get the full delay, slices it never lights are not delayed.
void Stilletchaser::ComputeDelays( const ChaseDocument& d, std::vector< float >& delays ) const
{
	delays.assign( d.slices.size(), 0.f );
	if( delayer <= 0.5f || d.sequences.empty() )
		return;
	const Sequence& seq = d.sequences[ Option( delaySequence, (int)d.sequences.size() ) ];
	const int count     = (int)seq.steps.size();
	const float full    = std::clamp( delay, 0.f, 1.f ) * MaxDelay();
	std::vector< int > firstStep( d.slices.size(), -1 );
	for( int i = 0; i < count; ++i )
		for( int slice : seq.steps[ i ] )
			if( firstStep[ slice ] < 0 )
				firstStep[ slice ] = i;
	for( size_t s = 0; s < d.slices.size(); ++s )
	{
		if( firstStep[ s ] < 0 )
			continue;
		float fraction = count > 1 ? (float)firstStep[ s ] / (float)( count - 1 ) : 0.f;
		if( reverse > 0.5f )
			fraction = 1.f - fraction;
		delays[ s ] = fraction * full;
	}
}

void Stilletchaser::UpdateHistory( const FFGLTextureStruct& input, GLuint hostFBO )
{
	const auto now = std::chrono::steady_clock::now();
	if( hasLastFrame )
	{
		double seconds = std::chrono::duration< double >( now - lastFrame ).count();
		// After a long pause the stored frames are stale: start over.
		if( autoReset > 0.5f && seconds > resetMinutes * 60.0 )
			history.Clear();
		if( seconds > 0.0 && seconds < 0.5 )
			framesPerSecond = framesPerSecond * 0.95f + (float)( 1.0 / seconds ) * 0.05f;
	}
	lastFrame    = now;
	hasLastFrame = true;
	frameTime    = std::chrono::duration< double >( now.time_since_epoch() ).count();

	if( delayer <= 0.5f )
	{
		if( history.Capacity() )
			history.Configure( 1, 1, 1 );// Give the VRAM back while the Delayer is off.
		return;
	}
	const float q    = std::clamp( quality, 0.1f, 1.f );
	const int width  = std::max( 1, (int)std::lround( input.Width * q ) );
	const int height = std::max( 1, (int)std::lround( input.Height * q ) );

	// Frames needed to cover the full delay. For delays in time this follows the frame rate,
	// which wobbles, so allocate with headroom in blocks and only resize on real changes:
	// resizing every frame would make the video stutter.
	int capacity = history.Capacity();
	if( DelayInFrames() )
		capacity = (int)MaxDelay() + 1;
	else
	{
		const int needed  = (int)std::ceil( MaxDelay() * framesPerSecond ) + 2;
		const int roomy   = ( (int)( needed * 1.25f ) / 16 + 1 ) * 16;
		if( capacity < needed || capacity > 2 * roomy )
			capacity = roomy;
	}
	capacity = std::clamp( capacity, 1, MAX_HISTORY + 1 );
	const size_t frameBytes = (size_t)width * height * 4;
	if( frameBytes * capacity > MAX_HISTORY_VRAM )
	{
		capacity = std::max( 2, (int)( MAX_HISTORY_VRAM / frameBytes ) );
		static bool warned = false;
		if( !warned )
			FFGLLog::LogToHost( "Stilletchaser: Delayer limited to 3 GB of video memory; lower Quality or the maximum delay" );
		warned = true;
	}
	history.Configure( capacity, width, height );
	history.Push( input, hostFBO, frameTime );
}

// Every slice, lit or not: the Wiper can run through any of them.
void Stilletchaser::BuildVertices( const ChaseDocument& d, const std::vector< float >& delays, const FFGLTextureStruct& input )
{
	vertices.clear();
	draws.clear();
	const int mode            = Option( scaling, 3 );
	const float contentAspect = input.Height ? (float)input.Width / (float)input.Height : 1.f;
	const bool delaying       = delayer > 0.5f && history.Filled() > 0;

	for( size_t s = 0; s < d.slices.size(); ++s )
	{
		const Slice& slice = d.slices[ s ];
		SliceDraw draw{ (GLint)( vertices.size() / FLOATS_PER_VERTEX ), (GLsizei)slice.indices.size(), input.Handle, false };
		if( delaying )
		{
			GLuint frame = DelayInFrames() ? history.Frame( (int)std::lround( delays[ s ] ) ) : history.FrameAt( frameTime - delays[ s ] );
			draw         = { draw.first, draw.count, frame, true };
		}
		draws.push_back( draw );

		for( uint16_t i : slice.indices )
		{
			const Vec2 p     = slice.points[ i ];
			const Vec2 uv    = ContentUV( d, slice, p, mode, contentAspect );
			const Vec2 local = SliceLocal( slice, p );
			vertices.insert( vertices.end(), { p.x, p.y, uv.x, uv.y, local.x, local.y } );
		}
	}
}

void Stilletchaser::BindSliceTexture( const SliceDraw& draw, const FFGLTexCoords& inputMax )
{
	glBindTexture( GL_TEXTURE_2D, draw.texture );
	// History frames are stored at exactly their own size, so their whole area is image.
	if( draw.fromHistory )
		shader.Set( "maxUV", 1.f, 1.f );
	else
		shader.Set( "maxUV", inputMax.s, inputMax.t );
}

// The wipes of the wipe sequence running at this moment (none while the Wiper is off).
std::vector< ActiveWipe > Stilletchaser::CurrentWipes( const ChaseDocument& d ) const
{
	if( WipeParam( PID_WIPER ) <= 0.5f || d.sequences.empty() )
		return {};
	const Sequence& seq = d.sequences[ Option( WipeParam( PID_WIPE_SEQUENCE ), (int)d.sequences.size() ) ];
	return ActiveWipes( (int)seq.steps.size(), WipeParam( PID_WIPE_STEP ), (int)std::lround( WipeParam( PID_WIPE_LOOPS ) ),
						std::clamp( WipeParam( PID_WIPE_LENGTH ), 0.01f, 1.f ), WipeParam( PID_WIPE_OVERLAP ), WipeParam( PID_WIPE_ODD_EVEN ) > 0.5f );
}

// The Wiper module: wipe bands through the slices of the wipe sequence's running steps.
void Stilletchaser::DrawWipes( const ChaseDocument& d, const FFGLTexCoords& inputMax )
{
	const auto wipes = CurrentWipes( d );
	if( wipes.empty() )
		return;
	const Sequence& seq = d.sequences[ Option( WipeParam( PID_WIPE_SEQUENCE ), (int)d.sequences.size() ) ];
	const View& v       = d.views[ Option( chase.view, (int)d.views.size() ) ];
	const float length  = std::clamp( WipeParam( PID_WIPE_LENGTH ), 0.01f, 1.f );
	const Envelope* env = ChosenEnvelope( d, WipeParam( PID_WIPE_ENVELOPE ) );
	const bool colour   = Option( WipeParam( PID_WIPE_SOURCE ), 2 ) == SOURCE_COLOUR;
	const bool flip     = WipeParam( PID_WIPE_FLIP ) > 0.5f;
	const bool swap     = WipeParam( PID_WIPE_SWAP ) > 0.5f;

	shader.Set( "wipeMode", 1 );
	shader.Set( "wipeLength", length );
	shader.Set( "wipeUseCurve", env ? 1 : 0 );
	if( env )
		glUniform1fv( glGetUniformLocation( shader.GetGLID(), "wipeCurve" ), 256, env->lut.data() );
	shader.Set( "useColour", colour ? 1 : 0 );
	shader.Set( "colour", WipeParam( PID_WIPE_RED ), WipeParam( PID_WIPE_GREEN ), WipeParam( PID_WIPE_BLUE ) );
	// Video wipes show the same content as the chase: keep the brightest of the two.
	// Colour wipes are laid over it.
	if( colour )
	{
		glBlendEquation( GL_FUNC_ADD );
		glBlendFunc( GL_ONE, GL_ONE_MINUS_SRC_ALPHA );
	}

	for( const ActiveWipe& active : wipes )
	{
		shader.Set( "wipeHead", active.head );
		for( int slice : seq.steps[ active.stepIndex ] )
		{
			if( !v.includes[ slice ] )
				continue;
			const WipeDirection direction = BaseWipeDirection( d, d.slices[ slice ], swap );
			shader.Set( "wipeAlongY", direction.alongY ? 1 : 0 );
			shader.Set( "wipeReversed", ( direction.reversed != flip != active.odd ) ? 1 : 0 );
			BindSliceTexture( draws[ slice ], inputMax );
			glDrawArrays( GL_TRIANGLES, draws[ slice ].first, draws[ slice ].count );
		}
	}
	shader.Set( "useColour", 0 );
	shader.Set( "wipeMode", 0 );
}

// The Outliner module. Lit Slices follows the chase (an echo's outline fades with it) and the
// wipes (a slice is outlined while a wipe runs through it); All Slices outlines the whole view.
void Stilletchaser::DrawOutlines( const ChaseDocument& d, const std::vector< float >& levels )
{
	if( OutlineParam( PID_OUTLINER ) <= 0.5f )
		return;
	const View& v = d.views[ Option( chase.view, (int)d.views.size() ) ];
	std::vector< float > strength( d.slices.size(), 0.f );
	if( Option( OutlineParam( PID_OUTLINE_TARGET ), 2 ) == TARGET_ALL )
	{
		for( size_t s = 0; s < strength.size(); ++s )
			strength[ s ] = v.includes[ s ] ? 1.f : 0.f;
	}
	else
	{
		strength = levels;
		const auto wipes = CurrentWipes( d );
		if( !wipes.empty() )
		{
			const Sequence& seq = d.sequences[ Option( WipeParam( PID_WIPE_SEQUENCE ), (int)d.sequences.size() ) ];
			const float length  = std::clamp( WipeParam( PID_WIPE_LENGTH ), 0.01f, 1.f );
			for( const ActiveWipe& active : wipes )
			{
				// Before it enters or after its tail has left, the band shows nothing.
				if( active.head <= 0.f || active.head - length >= 1.f )
					continue;
				for( int slice : seq.steps[ active.stepIndex ] )
					if( v.includes[ slice ] )
						strength[ slice ] = 1.f;
			}
		}
	}

	OutlineStyle style;
	style.red       = OutlineParam( PID_OUTLINE_RED );
	style.green     = OutlineParam( PID_OUTLINE_GREEN );
	style.blue      = OutlineParam( PID_OUTLINE_BLUE );
	style.width     = OutlineParam( PID_OUTLINE_WIDTH );
	style.feather   = OutlineParam( PID_OUTLINE_FEATHER );
	style.animate   = OutlineParam( PID_OUTLINE_ANIMATE ) > 0.5f;
	style.phase     = OutlineParam( PID_OUTLINE_PHASE );
	style.fan       = OutlineParam( PID_OUTLINE_FAN );
	style.randomise = OutlineParam( PID_OUTLINE_RANDOMISE );
	style.length    = OutlineParam( PID_OUTLINE_LENGTH );
	style.count     = (int)std::lround( OutlineParam( PID_OUTLINE_COUNT ) );
	style.flip      = OutlineParam( PID_OUTLINE_FLIP ) > 0.5f;
	style.oddEven   = OutlineParam( PID_OUTLINE_ODD_EVEN ) > 0.5f;
	style.envelope  = ChosenEnvelope( d, OutlineParam( PID_OUTLINE_ENVELOPE ) );
	outlines.Draw( d, style, strength, nullptr );
}

FFResult Stilletchaser::ProcessOpenGL( ProcessOpenGLStruct* pGL )
{
	if( pGL->numInputTextures < 1 || pGL->inputTextures[ 0 ] == nullptr )
		return FF_FAIL;
	const FFGLTextureStruct& input = *pGL->inputTextures[ 0 ];

	DocumentWatcher& watcher = DocumentWatcher::Instance();
	doc = watcher.Current();
	if( watcher.Version() != docVersion )
	{
		docVersion = watcher.Version();
		RefreshMenus( true );
	}

	// Record the frame even when nothing is lit, so the delay is ready when slices light up.
	UpdateHistory( input, pGL->HostFBO );

	glClearColor( 0.f, 0.f, 0.f, 0.f );
	glClear( GL_COLOR_BUFFER_BIT );
	if( doc->slices.empty() )
		return FF_SUCCESS;

	std::vector< float > levels;
	std::vector< float > delays;
	chase.bpm = bpm;// The host's tempo, for Bump Sync.
	chase.Levels( *doc, levels );
	ComputeDelays( *doc, delays );
	BuildVertices( *doc, delays, input );
	if( vertices.empty() )
		return FF_SUCCESS;

	// Outlines Only leaves out the chase and wipe content and keeps just the outlines.
	const bool content = OutlineParam( PID_OUTLINER ) <= 0.5f || OutlineParam( PID_OUTLINES_ONLY ) <= 0.5f;
	if( content )
	{
		ScopedShaderBinding shaderBinding( shader.GetGLID() );
		ScopedSamplerActivation activateSampler( 0 );
		Scoped2DTextureBinding textureBinding( input.Handle );// Restores the texture binding afterwards.
		shader.Set( "inputTexture", 0 );
		shader.Set( "wipeMode", 0 );
		shader.Set( "useColour", 0 );
		const FFGLTexCoords inputMax = GetMaxGLTexCoords( input );

		ScopedVAOBinding vaoBinding( vao );
		ScopedVBOBinding vboBinding( vbo );
		glBufferData( GL_ARRAY_BUFFER, vertices.size() * sizeof( float ), vertices.data(), GL_STREAM_DRAW );
		glEnable( GL_BLEND );
		glBlendEquation( GL_MAX );
		for( size_t s = 0; s < draws.size(); ++s )
		{
			if( levels[ s ] <= 0.f )
				continue;
			shader.Set( "level", levels[ s ] );
			BindSliceTexture( draws[ s ], inputMax );
			glDrawArrays( GL_TRIANGLES, draws[ s ].first, draws[ s ].count );
		}
		DrawWipes( *doc, inputMax );
		glBlendEquation( GL_FUNC_ADD );
		glBlendFunc( GL_ONE, GL_ZERO );
		glDisable( GL_BLEND );
	}
	DrawOutlines( *doc, levels );
	return FF_SUCCESS;
}

FFResult Stilletchaser::SetFloatParameter( unsigned int index, float value )
{
	switch( index )
	{
	case PID_DRIVER:
		chase.driver = value;
		// A Bump means a step under Bumper and a slice under Random, so nothing carries over.
		chase.Reset();
		UpdateVisibility( true );
		break;
	case PID_SEQUENCE:
		chase.sequence = value;
		UpdateSequenceButtons( true );
		break;
	case PID_STEP:     chase.step = value; break;
	case PID_SCALING:  scaling = value; break;
	case PID_VIEW:     chase.view = value; break;
	case PID_ECHOES:   chase.echoes = value; break;
	case PID_ENVELOPE:
		chase.envelope = value;
		UpdateVisibility( true );
		break;
	case PID_SUSTAIN:  chase.sustain = value; break;
	case PID_RELEASE:  chase.release = value; break;
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
	case PID_RANDOM_MAX:   chase.randomMax = value; break;
	case PID_RANDOM_SPEED: chase.randomSpeed = value; break;
	case PID_BUMP_PREV:
		if( value > 0.5f )
			chase.Tap( -1 );
		break;
	case PID_BUMP_NEXT:
		if( value > 0.5f )
			chase.Tap( 1 );
		break;
	case PID_BUMP_BLAST:
		// Pressing starts it and letting go stops it. Should the host not report the release,
		// a second press stops it instead, so the button works either way.
		chase.SetBlast( value > 0.5f ? !chase.Blasting() : false );
		break;
	case PID_DELAYER:
		delayer = value;
		UpdateVisibility( true );
		break;
	case PID_DELAY_SEQUENCE: delaySequence = value; break;
	case PID_DELAY:          delay = value; break;
	case PID_DELAY_UNIT:
		delayUnit = value;
		UpdateVisibility( true );
		break;
	case PID_MAX_FRAMES:  maxFrames = value; break;
	case PID_MAX_SECONDS: maxSeconds = value; break;
	case PID_MAX_BEATS:   maxBeats = value; break;
	case PID_QUALITY:     quality = value; break;
	case PID_REVERSE:     reverse = value; break;
	case PID_AUTO_RESET:
		autoReset = value;
		UpdateVisibility( true );
		break;
	case PID_RESET_MINUTES: resetMinutes = value; break;
	default:
		if( index >= PID_WIPER && index < PID_WIPER + WIPE_PARAMS )
		{
			wipe[ index - PID_WIPER ] = value;
			if( index == PID_WIPER || index == PID_WIPE_SOURCE )
				UpdateVisibility( true );
		}
		else if( index >= PID_OUTLINER && index < PID_OUTLINER + OUTLINE_PARAMS )
		{
			outline[ index - PID_OUTLINER ] = value;
			if( index == PID_OUTLINER || index == PID_OUTLINE_ANIMATE )
				UpdateVisibility( true );
		}
		else if( index >= PID_SEQUENCE_BUTTON && index < PID_SEQUENCE_BUTTON + SEQUENCE_BUTTONS )
		{
			// A press (not the release) chooses the sequence; the menu's value follows, for the host to show.
			const int chosen = (int)( index - PID_SEQUENCE_BUTTON );
			if( value > 0.5f && chosen < (int)doc->sequences.size() )
			{
				chase.sequence = (float)chosen;
				RaiseParamEvent( PID_SEQUENCE, FF_EVENT_FLAG_VALUE );
				UpdateSequenceButtons( true );
				// A different sequence has different steps, so the pads follow and the tapping restarts.
				chase.Reset();
				UpdateBumpButtons( true );
			}
		}
		else if( index >= PID_BUMP_BUTTON && index < PID_BUMP_BUTTON + BUMP_BUTTONS )
		{
			// The press fires the step; the release only ends it when Hold is on.
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

float Stilletchaser::GetFloatParameter( unsigned int index )
{
	switch( index )
	{
	case PID_DRIVER:         return chase.driver;
	case PID_SEQUENCE:       return chase.sequence;
	case PID_STEP:           return chase.step;
	case PID_SCALING:        return scaling;
	case PID_VIEW:           return chase.view;
	case PID_ECHOES:         return chase.echoes;
	case PID_ENVELOPE:       return chase.envelope;
	case PID_SUSTAIN:        return chase.sustain;
	case PID_RELEASE:        return chase.release;
	case PID_BUMP_HOLD:      return chase.bumpHold;
	case PID_BUMP_LENGTH:    return chase.bumpLength;
	case PID_BUMP_SYNC:      return chase.bumpSync;
	case PID_BLAST_SPEED:    return chase.blastSpeed;
	case PID_BUMP_AUTO_RESET: return chase.bumpAutoReset;
	case PID_BUMP_RESET_DELAY: return chase.bumpResetDelay;
	case PID_RANDOM_MAX:     return chase.randomMax;
	case PID_RANDOM_SPEED:   return chase.randomSpeed;
	case PID_DELAYER:        return delayer;
	case PID_DELAY_SEQUENCE: return delaySequence;
	case PID_DELAY:          return delay;
	case PID_DELAY_UNIT:     return delayUnit;
	case PID_MAX_FRAMES:     return maxFrames;
	case PID_MAX_SECONDS:    return maxSeconds;
	case PID_MAX_BEATS:      return maxBeats;
	case PID_QUALITY:        return quality;
	case PID_REVERSE:        return reverse;
	case PID_AUTO_RESET:     return autoReset;
	case PID_RESET_MINUTES:  return resetMinutes;
	default:
		if( index >= PID_WIPER && index < PID_WIPER + WIPE_PARAMS )
			return wipe[ index - PID_WIPER ];
		if( index >= PID_OUTLINER && index < PID_OUTLINER + OUTLINE_PARAMS )
			return outline[ index - PID_OUTLINER ];
		return 0.f;
	}
}

char* Stilletchaser::GetParameterDisplay( unsigned int index )
{
	static char display[ 48 ];
	if( ( index == PID_STEP || index == PID_WIPE_STEP ) && doc && !doc->sequences.empty() )
	{
		const bool wipeStep = index == PID_WIPE_STEP;
		const Sequence& seq = doc->sequences[ Option( wipeStep ? WipeParam( PID_WIPE_SEQUENCE ) : chase.sequence, (int)doc->sequences.size() ) ];
		int count           = (int)seq.steps.size();
		float position      = wipeStep ? WipeParam( PID_WIPE_STEP ) : chase.step;
		int current         = std::min( (int)( position * (float)count ), count - 1 ) + 1;
		std::snprintf( display, sizeof( display ), "%d / %d", current, count );
		return display;
	}
	if( index == PID_DELAY )
	{
		const float full = std::clamp( delay, 0.f, 1.f ) * MaxDelay();
		if( DelayInFrames() )
			std::snprintf( display, sizeof( display ), "%d frames", (int)std::lround( full ) );
		else
			std::snprintf( display, sizeof( display ), "%.2f s", full );
		return display;
	}
	if( index == PID_BUMP_LENGTH )
	{
		if( chase.bumpSync > 0.5f )
			std::snprintf( display, sizeof( display ), "%.2f beats", std::clamp( chase.bumpLength, 0.02f, 5.f ) );
		else
			std::snprintf( display, sizeof( display ), "%.2f s", std::clamp( chase.bumpLength, 0.02f, 5.f ) );
		return display;
	}
	if( index == PID_RANDOM_SPEED )
	{
		std::snprintf( display, sizeof( display ), "%.1f picks/s", std::clamp( chase.randomSpeed, 0.1f, 30.f ) );
		return display;
	}
	if( index == PID_RANDOM_MAX )
	{
		std::snprintf( display, sizeof( display ), "up to %d", std::max( 1, (int)std::lround( chase.randomMax ) ) );
		return display;
	}
	if( index == PID_BLAST_SPEED )
	{
		std::snprintf( display, sizeof( display ), "%.1f steps/s", std::clamp( chase.blastSpeed, 1.f, 50.f ) );
		return display;
	}
	if( index == PID_QUALITY )
	{
		std::snprintf( display, sizeof( display ), "%d%%", (int)std::lround( quality * 100.f ) );
		return display;
	}
	return CFFGLPlugin::GetParameterDisplay( index );
}
