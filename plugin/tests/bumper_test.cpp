// Drives the Bumper driver without a GPU: builds a small document by hand, fires pads through
// SetFloatParameter and checks which slices the chase lights and how they fade.
// The plugin keeps its state private, so the test reaches in; nothing else does.
// The .cpp is included so the test can see the parameter ids, which are local to it.
#include "../src/Stilletchaser.cpp"

#include <cstdio>
#include <thread>

using namespace stilletchaser;

// Declared a friend in Stilletchaser.h, so the checks can look at the plugin's own state.
struct StilletchaserTest
{
	static int Run();
};

static std::shared_ptr< ChaseDocument > FourSliceDocument()
{
	auto d        = std::make_shared< ChaseDocument >();
	d->compWidth  = 1920.f;
	d->compHeight = 1080.f;
	for( int i = 0; i < 4; ++i )
	{
		Slice s;
		s.id    = "slice" + std::to_string( i );
		s.name  = s.id;
		s.origin = { 0.25f * i, 0.f };
		s.axisX  = { 0.25f, 0.f };
		s.axisY  = { 0.f, 1.f };
		d->slices.push_back( s );
	}
	Sequence seq;
	seq.id    = "seq";
	seq.name  = "Four";
	seq.steps = { { 0 }, { 1 }, { 2 }, { 3 } };
	d->sequences.push_back( seq );
	View all;
	all.id       = "all";
	all.name     = "All";
	all.includes = std::vector< bool >( d->slices.size(), true );
	d->views.push_back( all );
	return d;
}

static void Sleep( double seconds )
{
	std::this_thread::sleep_for( std::chrono::duration< double >( seconds ) );
}

static int failures = 0;
static void Check( bool ok, const char* what )
{
	std::printf( "%s %s\n", ok ? "ok  " : "FAIL", what );
	if( !ok )
		++failures;
}

int StilletchaserTest::Run()
{
	Stilletchaser plugin;
	plugin.doc = FourSliceDocument();
	std::vector< float > levels;

	// Bumper lights nothing until a pad is hit.
	plugin.SetFloatParameter( PID_DRIVER, (float)DRIVER_BUMPER );
	plugin.SetFloatParameter( PID_BUMP_LENGTH, 0.4f );
	plugin.SetFloatParameter( PID_SUSTAIN, 0.25f );
	plugin.SetFloatParameter( PID_RELEASE, 1.f );
	plugin.chase.Levels( *plugin.doc, levels );
	Check( levels.size() == 4 && levels[ 0 ] == 0.f && levels[ 3 ] == 0.f, "silent until a pad is pressed" );

	// Pad 3 lights only step 3's slice, at full opacity while inside Sustain.
	plugin.SetFloatParameter( PID_BUMP_BUTTON + 2, 1.f );
	plugin.chase.Levels( *plugin.doc, levels );
	Check( levels[ 2 ] == 1.f, "the pad's own step lights" );
	Check( levels[ 0 ] == 0.f && levels[ 1 ] == 0.f && levels[ 3 ] == 0.f, "no other step lights" );

	// Releasing the pad does nothing with Hold off: the bump plays out on its own.
	plugin.SetFloatParameter( PID_BUMP_BUTTON + 2, 0.f );
	Sleep( 0.2 );// Half of Bump Length: past Sustain (0.25), inside the Release ramp.
	plugin.chase.Levels( *plugin.doc, levels );
	Check( levels[ 2 ] > 0.f && levels[ 2 ] < 1.f, "it fades through the release ramp" );
	const float midway = levels[ 2 ];

	Sleep( 0.3 );// Past Bump Length: over.
	plugin.chase.Levels( *plugin.doc, levels );
	Check( levels[ 2 ] == 0.f, "it ends after Bump Length" );
	Check( plugin.chase.bumps.empty(), "the finished bump is dropped" );

	// Echoes: the bump trails over the steps before it, each further along the curve.
	plugin.SetFloatParameter( PID_ECHOES, 2.f );
	plugin.SetFloatParameter( PID_BUMP_BUTTON + 2, 1.f );
	plugin.SetFloatParameter( PID_BUMP_BUTTON + 2, 0.f );
	plugin.chase.Levels( *plugin.doc, levels );
	Check( levels[ 2 ] >= levels[ 1 ] && levels[ 1 ] >= levels[ 0 ], "echoes trail off behind the pad" );
	Check( levels[ 1 ] > 0.f, "the trail actually lights" );
	Check( levels[ 3 ] == 0.f, "the trail runs backwards, not forwards" );
	Sleep( 0.5 );
	plugin.chase.Levels( *plugin.doc, levels );
	Check( levels[ 2 ] > 0.f, "with echoes the head lives longer than one Bump Length" );

	// Two pads at once, and the trail wrapping around the start of the sequence.
	plugin.SetFloatParameter( PID_ECHOES, 0.f );
	plugin.chase.bumps.clear();
	plugin.SetFloatParameter( PID_BUMP_BUTTON + 0, 1.f );
	plugin.SetFloatParameter( PID_BUMP_BUTTON + 3, 1.f );
	plugin.chase.Levels( *plugin.doc, levels );
	Check( levels[ 0 ] == 1.f && levels[ 3 ] == 1.f && levels[ 1 ] == 0.f, "pads stack" );
	Check( plugin.chase.bumps.size() == 2, "both bumps are held" );

	// Hold: the bump waits at Sustain for as long as the pad is down.
	plugin.chase.bumps.clear();
	plugin.SetFloatParameter( PID_BUMP_HOLD, 1.f );
	plugin.SetFloatParameter( PID_BUMP_BUTTON + 1, 1.f );
	Sleep( 0.6 );// Well past Bump Length.
	plugin.chase.Levels( *plugin.doc, levels );
	Check( levels[ 1 ] == 1.f, "a held bump stays lit past Bump Length" );
	plugin.SetFloatParameter( PID_BUMP_BUTTON + 1, 0.f );
	Sleep( 0.1 );
	plugin.chase.Levels( *plugin.doc, levels );
	Check( levels[ 1 ] > 0.f && levels[ 1 ] < 1.f, "letting go starts the release" );
	Sleep( 0.4 );
	plugin.chase.Levels( *plugin.doc, levels );
	Check( levels[ 1 ] == 0.f, "and it finishes" );

	// Re-pressing a sounding pad restarts it instead of stacking a copy.
	plugin.SetFloatParameter( PID_BUMP_HOLD, 0.f );
	plugin.chase.bumps.clear();
	plugin.SetFloatParameter( PID_BUMP_BUTTON + 0, 1.f );
	plugin.SetFloatParameter( PID_BUMP_BUTTON + 0, 0.f );
	Sleep( 0.25 );
	plugin.SetFloatParameter( PID_BUMP_BUTTON + 0, 1.f );
	plugin.chase.Levels( *plugin.doc, levels );
	Check( plugin.chase.bumps.size() == 1, "re-pressing does not stack" );
	Check( levels[ 0 ] == 1.f, "re-pressing restarts the curve" );

	// Bump Sync reads the length in beats at the host's tempo.
	plugin.chase.bpm = 120.f;// 0.5 s per beat.
	plugin.SetFloatParameter( PID_BUMP_LENGTH, 1.f );
	plugin.SetFloatParameter( PID_BUMP_SYNC, 1.f );
	Check( std::fabs( plugin.chase.BumpLength() - 0.5f ) < 1e-5f, "one beat at 120 bpm is half a second" );
	plugin.SetFloatParameter( PID_BUMP_SYNC, 0.f );
	Check( std::fabs( plugin.chase.BumpLength() - 1.f ) < 1e-5f, "without sync it is seconds" );

	// Next and Previous tap through the sequence; the first tap of a phrase lands on step 1.
	plugin.SetFloatParameter( PID_BUMP_SYNC, 0.f );
	plugin.SetFloatParameter( PID_BUMP_LENGTH, 0.4f );
	plugin.SetFloatParameter( PID_BUMP_AUTO_RESET, 0.f );
	plugin.chase.bumps.clear();
	plugin.chase.cursor = -1;
	plugin.SetFloatParameter( PID_BUMP_NEXT, 1.f );
	plugin.chase.Levels( *plugin.doc, levels );
	Check( plugin.chase.cursor == 0 && levels[ 0 ] == 1.f, "the first Next starts at step 1" );
	Sleep( 0.2 );// Half of Bump Length, so step 1 is past Sustain by the time step 2 is tapped.
	plugin.SetFloatParameter( PID_BUMP_NEXT, 1.f );
	plugin.chase.Levels( *plugin.doc, levels );
	Check( plugin.chase.cursor == 1 && levels[ 1 ] == 1.f, "Next walks forward" );
	Check( levels[ 0 ] > 0.f && levels[ 0 ] < 1.f, "the step tapped off is still fading" );
	plugin.SetFloatParameter( PID_BUMP_PREV, 1.f );
	Check( plugin.chase.cursor == 0, "Previous walks back" );
	plugin.SetFloatParameter( PID_BUMP_PREV, 1.f );
	Check( plugin.chase.cursor == 3, "Previous wraps round the start" );
	plugin.SetFloatParameter( PID_BUMP_NEXT, 1.f );
	Check( plugin.chase.cursor == 0, "Next wraps round the end" );

	// A pad moves the cursor, so tapping carries on from the step just hit.
	plugin.SetFloatParameter( PID_BUMP_BUTTON + 2, 1.f );
	plugin.SetFloatParameter( PID_BUMP_BUTTON + 2, 0.f );
	Check( plugin.chase.cursor == 2, "a pad moves the tapping cursor" );
	plugin.SetFloatParameter( PID_BUMP_NEXT, 1.f );
	Check( plugin.chase.cursor == 3, "Next carries on from the pad" );

	// Auto Reset puts the tapping back to the start after a quiet spell.
	plugin.SetFloatParameter( PID_BUMP_AUTO_RESET, 1.f );
	plugin.SetFloatParameter( PID_BUMP_RESET_DELAY, 0.2f );
	plugin.chase.Levels( *plugin.doc, levels );
	Check( plugin.chase.cursor == 3, "it does not reset while still being played" );
	Sleep( 0.35 );
	plugin.chase.Levels( *plugin.doc, levels );
	Check( plugin.chase.cursor == -1, "it resets after Reset Delay" );
	plugin.SetFloatParameter( PID_BUMP_NEXT, 1.f );
	Check( plugin.chase.cursor == 0, "and the phrase starts again from step 1" );
	plugin.SetFloatParameter( PID_BUMP_AUTO_RESET, 0.f );
	plugin.SetFloatParameter( PID_BUMP_NEXT, 1.f );
	Sleep( 0.35 );
	plugin.chase.Levels( *plugin.doc, levels );
	Check( plugin.chase.cursor == 1, "with Auto Reset off it stays where it was" );

	// Blast runs the sequence on by itself at Blast Speed.
	plugin.chase.bumps.clear();
	plugin.chase.cursor = -1;
	plugin.SetFloatParameter( PID_BUMP_LENGTH, 2.f );// Long, so nothing dies during the roll.
	plugin.SetFloatParameter( PID_BLAST_SPEED, 20.f );// 20 steps a second.
	plugin.SetFloatParameter( PID_BUMP_BLAST, 1.f );
	Check( plugin.chase.blasting, "pressing Blast starts it" );
	plugin.chase.Levels( *plugin.doc, levels );
	Check( plugin.chase.cursor == 0, "the Blast fires its first step at once" );
	Sleep( 0.25 );// Five steps' worth: past one full pass of four.
	plugin.chase.Levels( *plugin.doc, levels );
	Check( levels[ 0 ] > 0.f && levels[ 1 ] > 0.f && levels[ 2 ] > 0.f && levels[ 3 ] > 0.f, "the Blast runs through every step" );
	plugin.SetFloatParameter( PID_BUMP_BLAST, 0.f );
	Check( !plugin.chase.blasting, "letting go stops the Blast" );
	const int restingCursor = plugin.chase.cursor;
	Sleep( 0.15 );
	plugin.chase.Levels( *plugin.doc, levels );
	Check( plugin.chase.cursor == restingCursor, "and it stops advancing" );

	// Without a release the second press stops it, so the button works either way.
	plugin.SetFloatParameter( PID_BUMP_BLAST, 1.f );
	Check( plugin.chase.blasting, "pressing again restarts it" );
	plugin.SetFloatParameter( PID_BUMP_BLAST, 1.f );
	Check( !plugin.chase.blasting, "a second press stops it when no release arrives" );

	// Tapping takes over from a Blast.
	plugin.SetFloatParameter( PID_BUMP_BLAST, 1.f );
	plugin.SetFloatParameter( PID_BUMP_NEXT, 1.f );
	Check( !plugin.chase.blasting, "Next stops a running Blast" );

	// Random picks slices out of the view by itself, without needing a sequence.
	plugin.SetFloatParameter( PID_DRIVER, (float)DRIVER_RANDOM );
	plugin.SetFloatParameter( PID_ECHOES, 0.f );
	plugin.SetFloatParameter( PID_SUSTAIN, 0.9f );
	plugin.SetFloatParameter( PID_RANDOM_MAX, 1.f );
	plugin.SetFloatParameter( PID_RANDOM_SPEED, 30.f );// A pick every 0.033 s.
	Check( plugin.chase.bumps.empty(), "switching driver clears what the last one was playing" );
	plugin.chase.Levels( *plugin.doc, levels );
	int lit = 0;
	for( float l : levels )
		if( l > 0.f )
			++lit;
	Check( lit == 1, "Max Slices of 1 lights exactly one slice" );

	// Over many picks every slice of the view should come up, and none outside it.
	// Enough picks that missing one by chance is vanishingly unlikely rather than occasional.
	std::vector< bool > seen( 4, false );
	for( int i = 0; i < 150; ++i )
	{
		Sleep( 0.015 );
		plugin.chase.Levels( *plugin.doc, levels );
		for( size_t s = 0; s < levels.size(); ++s )
			if( levels[ s ] > 0.f )
				seen[ s ] = true;
	}
	Check( seen[ 0 ] && seen[ 1 ] && seen[ 2 ] && seen[ 3 ], "every slice of the view gets picked" );

	// Max Slices raises how many can be lit at once.
	plugin.chase.bumps.clear();
	plugin.chase.randomNext = 0.0;
	plugin.SetFloatParameter( PID_RANDOM_MAX, 4.f );
	plugin.SetFloatParameter( PID_RANDOM_SPEED, 8.f );
	// A pick takes 1 to Max Slices slices, so watch enough of them to be sure of seeing a big one.
	int most = 0;
	for( int i = 0; i < 100; ++i )
	{
		Sleep( 0.02 );
		plugin.chase.Levels( *plugin.doc, levels );
		int now = 0;
		for( float l : levels )
			if( l > 0.f )
				++now;
		most = std::max( most, now );
	}
	Check( most > 1, "Max Slices above 1 lights several at once" );
	Check( most <= 4, "it never lights more slices than the view holds" );

	// A view that excludes a slice must never light it. The plugin holds the document as const,
	// so the test swaps in its own copy.
	auto full = plugin.doc;
	auto trimmed = std::make_shared< ChaseDocument >( *full );
	trimmed->views[ 0 ].includes[ 1 ] = false;
	plugin.doc = trimmed;
	plugin.chase.bumps.clear();
	plugin.chase.randomNext = 0.0;
	bool leaked = false;
	for( int i = 0; i < 40; ++i )
	{
		Sleep( 0.02 );
		plugin.chase.Levels( *plugin.doc, levels );
		if( levels[ 1 ] > 0.f )
			leaked = true;
	}
	Check( !leaked, "Random stays inside the View" );
	plugin.doc = full;

	// Picks fade rather than cutting off, so look at one halfway through its life.
	plugin.SetFloatParameter( PID_SUSTAIN, 0.1f );
	plugin.SetFloatParameter( PID_RELEASE, 1.f );
	plugin.SetFloatParameter( PID_RANDOM_SPEED, 5.f );// A pick every 0.2 s.
	plugin.chase.bumps.clear();
	plugin.chase.randomNext = 0.0;
	plugin.chase.Levels( *plugin.doc, levels );// Fires one pick, at full.
	Sleep( 0.1 );// Half its life, and before the next pick is due.
	plugin.chase.Levels( *plugin.doc, levels );
	bool fading = false;
	for( float l : levels )
		if( l > 0.f && l < 1.f )
			fading = true;
	Check( fading, "picks fade rather than cutting off" );

	// Random needs no sequence at all.
	auto bare = std::make_shared< ChaseDocument >( *full );
	bare->sequences.clear();
	plugin.doc = bare;
	plugin.chase.bumps.clear();
	plugin.chase.randomNext = 0.0;
	plugin.chase.Levels( *plugin.doc, levels );
	lit = 0;
	for( float l : levels )
		if( l > 0.f )
			++lit;
	Check( lit > 0, "Random runs with no sequences defined" );
	plugin.doc = full;

	// The other drivers still behave.
	plugin.SetFloatParameter( PID_DRIVER, (float)DRIVER_NONE );
	plugin.chase.Levels( *plugin.doc, levels );
	Check( levels[ 0 ] == 1.f && levels[ 1 ] == 1.f && levels[ 2 ] == 1.f && levels[ 3 ] == 1.f, "None still lights every slice" );
	plugin.SetFloatParameter( PID_DRIVER, (float)DRIVER_CHASER );
	plugin.SetFloatParameter( PID_ECHOES, 0.f );
	plugin.SetFloatParameter( PID_STEP, 0.6f );// Into step 3 of 4.
	plugin.chase.Levels( *plugin.doc, levels );
	Check( levels[ 2 ] > 0.f && levels[ 0 ] == 0.f, "Chaser still follows Step" );

	std::printf( failures ? "\n%d check(s) failed\n" : "\nall checks passed\n", failures );
	return failures ? 1 : 0;
}

int main()
{
	return StilletchaserTest::Run();
}
