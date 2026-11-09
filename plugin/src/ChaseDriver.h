#pragma once
#include <mutex>
#include <random>
#include <vector>
#include "ChaseDocument.h"

struct StilletchaserTest;// plugin/tests/bumper_test.cpp

namespace stilletchaser
{
// Which slices are lit, and how brightly. Shared by STLT-CHASER, which uses the levels as the
// opacity of the content, and STLT-BLEND, which uses them to mix between two textures.
//
// Chaser   a sequence stepped by Step, which Resolume animates with BPM sync, the timeline or FFT
// Bumper   steps fired by hand from the pads, or tapped with Next and Previous, each one playing
//          the opacity curve over Bump Length
// Random   slices of the view picked at random, Random Speed times a second
// None     every slice of the view, always
enum Driver
{
	DRIVER_CHASER,
	DRIVER_NONE,
	DRIVER_BUMPER,
	DRIVER_RANDOM,
};
const int DRIVERS    = 4;
const int MAX_ECHOES = 32;

class ChaseDriver
{
public:
	// ---- Settings, written straight from the host's parameters ----
	float driver   = (float)DRIVER_CHASER;
	float sequence = 0.f;
	float step     = 0.f;
	float view     = 0.f;
	float echoes   = 0.f;
	float envelope = 0.f;
	float sustain  = 0.2f;
	float release  = 0.8f;

	float bumpHold       = 0.f;
	float bumpLength     = 0.5f;
	float bumpSync       = 0.f;
	float blastSpeed     = 12.f;
	float bumpAutoReset  = 1.f;
	float bumpResetDelay = 2.f;

	float randomMax   = 3.f;
	float randomSpeed = 4.f;

	float bpm = 120.f;// The host's tempo, for Bump Sync.

	// ---- Playing ----

	// Opacity of every slice right now. Resizes `levels` to the document's slice count.
	void Levels( const ChaseDocument& doc, std::vector< float >& levels );

	// A pad: fires that step and moves the tapping cursor onto it.
	void StartBump( int step );
	// Letting a pad go, which only matters while Bump Hold is on.
	void EndBump( int step );
	// Next ( +1 ) and Previous ( -1 ): walk the sequence by hand, firing each step tapped onto.
	void Tap( int delta );
	void SetBlast( bool on );
	bool Blasting() const
	{
		return blasting;
	}
	// Everything the old driver was playing, dropped: a Bump means a step under Bumper and a
	// slice under Random, so nothing carries over when the driver or the sequence changes.
	void Reset();

	// How many steps the chosen sequence has, 0 when there is nothing to play.
	int StepCount( const ChaseDocument& doc ) const;
	// Which steps are sounding, for marking the pads. Up to `count` of them.
	std::vector< bool > Sounding( int count ) const;
	// True once since the last call if the pads need redrawing.
	bool TakePadsDirty();

	// How long one step of a bump lasts, in seconds (Bump Length, read as beats when Sync is on).
	float BumpLength() const;
	// Seconds on the steady clock, the clock everything here is timed against.
	static double Now();

private:
	friend struct ::StilletchaserTest;// plugin/tests/bumper_test.cpp

	// What one Bump holds depends on the driver: the sequence's step under Bumper, the slice
	// itself under Random.
	struct Bump
	{
		int step;
		double start;
		bool held;
		double released;
	};

	void BumpLevels( const ChaseDocument& doc, std::vector< float >& levels );
	void RandomLevels( const ChaseDocument& doc, std::vector< float >& levels );
	// Starts a step with mutex already held; the cursor and the pads are the caller's job.
	void StartLocked( int step );
	// Where a held bump waits in its curve, 0..1 of one step's life.
	float HoldPoint() const;

	// The list is touched from the host's parameter thread and read while rendering, so it is guarded.
	mutable std::mutex mutex;
	std::vector< Bump > bumps;
	// Where tapping with Next and Previous has got to, -1 before the first tap of a phrase.
	// The pads share it, so a pad jumps the tapping to that step and Next carries on from there.
	int cursor      = -1;
	double lastTap  = 0.0;// When a pad, Next, Previous or Blast was last used.
	bool blasting   = false;
	double blastNext = 0.0;// When the Blast fires its next step.
	bool padsDirty  = false;

	// Random: when the next handful of slices is picked, and how it is picked.
	double randomNext = 0.0;
	std::mt19937 rng{ std::random_device{}() };

	// The sequence's step count as of the last Levels(), so tapping works without the document.
	int knownSteps = 0;
};
}// namespace stilletchaser
