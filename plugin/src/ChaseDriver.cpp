#include "ChaseDriver.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include "PluginCommon.h"

namespace stilletchaser
{
double ChaseDriver::Now()
{
	using namespace std::chrono;
	return duration< double >( steady_clock::now().time_since_epoch() ).count();
}

float ChaseDriver::BumpLength() const
{
	const float length = std::clamp( bumpLength, 0.02f, 5.f );
	if( bumpSync > 0.5f )
		return length * 60.f / std::max( bpm, 1.f );
	return length;
}

float ChaseDriver::HoldPoint() const
{
	return std::clamp( sustain, 0.f, 0.99f );
}

int ChaseDriver::StepCount( const ChaseDocument& doc ) const
{
	if( doc.sequences.empty() )
		return 0;
	return (int)doc.sequences[ Option( sequence, (int)doc.sequences.size() ) ].steps.size();
}

std::vector< bool > ChaseDriver::Sounding( int count ) const
{
	std::vector< bool > on( std::max( count, 0 ), false );
	std::lock_guard< std::mutex > lock( mutex );
	for( const Bump& b : bumps )
		if( b.step >= 0 && b.step < count )
			on[ b.step ] = true;
	return on;
}

bool ChaseDriver::TakePadsDirty()
{
	std::lock_guard< std::mutex > lock( mutex );
	const bool was = padsDirty;
	padsDirty      = false;
	return was;
}

void ChaseDriver::Reset()
{
	std::lock_guard< std::mutex > lock( mutex );
	bumps.clear();
	cursor     = -1;
	blasting   = false;
	randomNext = 0.0;
	padsDirty  = true;
}

void ChaseDriver::StartLocked( int step )
{
	// Hitting a step that is still sounding restarts it rather than stacking a second copy.
	for( Bump& b : bumps )
		if( b.step == step )
		{
			b.start    = Now();
			b.held     = true;
			b.released = 0.0;
			return;
		}
	bumps.push_back( { step, Now(), true, 0.0 } );
}

void ChaseDriver::StartBump( int step )
{
	std::lock_guard< std::mutex > lock( mutex );
	// A pad also moves the tapping cursor, so Next carries on from the step just hit.
	cursor    = step;
	lastTap   = Now();
	padsDirty = true;
	StartLocked( step );
}

// Letting a pad go only matters in Hold: the bump leaves the sustain point and fades out.
void ChaseDriver::EndBump( int step )
{
	std::lock_guard< std::mutex > lock( mutex );
	for( Bump& b : bumps )
		if( b.step == step && b.held )
		{
			b.held     = false;
			b.released = Now();
		}
}

// Next and Previous walk the sequence by hand. The first tap of a phrase lands on the first step
// (or the last, going backwards) rather than stepping off it.
void ChaseDriver::Tap( int delta )
{
	std::lock_guard< std::mutex > lock( mutex );
	const int count = knownSteps;
	if( count <= 0 )
		return;
	blasting  = false;
	cursor    = cursor < 0 ? ( delta > 0 ? 0 : count - 1 ) : ( ( cursor + delta ) % count + count ) % count;
	lastTap   = Now();
	padsDirty = true;
	StartLocked( cursor );
}

void ChaseDriver::SetBlast( bool on )
{
	std::lock_guard< std::mutex > lock( mutex );
	blasting = on;
	lastTap  = Now();
	if( on )
		blastNext = 0.0;// Fire the first step on the next frame.
}

void ChaseDriver::Levels( const ChaseDocument& doc, std::vector< float >& levels )
{
	if( doc.views.empty() || doc.slices.empty() )
	{
		levels.assign( doc.slices.size(), 0.f );
		return;
	}
	const View& v   = doc.views[ Option( view, (int)doc.views.size() ) ];
	const int which = Option( driver, DRIVERS );
	{
		std::lock_guard< std::mutex > lock( mutex );
		knownSteps = StepCount( doc );
	}

	// Random lights slices of the view directly, so it runs before a sequence is needed.
	if( which == DRIVER_RANDOM )
	{
		levels.assign( doc.slices.size(), 0.f );
		RandomLevels( doc, levels );
		return;
	}
	if( which == DRIVER_NONE || doc.sequences.empty() )
	{
		levels.resize( doc.slices.size() );
		for( size_t s = 0; s < doc.slices.size(); ++s )
			levels[ s ] = which == DRIVER_NONE && v.includes[ s ] ? 1.f : 0.f;
		return;
	}
	levels.assign( doc.slices.size(), 0.f );
	if( which == DRIVER_BUMPER )
	{
		BumpLevels( doc, levels );
		return;
	}

	const Sequence& seq = doc.sequences[ Option( sequence, (int)doc.sequences.size() ) ];
	const int count     = (int)seq.steps.size();
	if( count <= 0 )
		return;
	const Envelope* env = ChosenEnvelope( doc, envelope );
	const int life      = 1 + std::clamp( (int)std::lround( echoes ), 0, MAX_ECHOES );
	const float pos     = std::clamp( step, 0.f, 0.999999f ) * (float)count;
	const int current   = std::min( (int)pos, count - 1 );
	const float within  = pos - (float)current;
	for( int age = 0; age < life; ++age )
	{
		const int index     = ( ( current - age ) % count + count ) % count;
		const float x       = ( within + (float)age ) / (float)life;
		const float opacity = env ? SampleLut( env->lut, x ) : SustainRelease( sustain, std::max( release, sustain ), x );
		if( opacity <= 0.f )
			continue;
		for( int slice : seq.steps[ index ] )
			if( v.includes[ slice ] )
				levels[ slice ] = std::max( levels[ slice ], opacity );
	}
}

// A bump plays the same curve the Chaser gives a step, over Bump Length instead of one step of
// the Step parameter. With Echoes it also lights the steps before it, further along that curve,
// so the chase it would have made trails out behind the pad. Held bumps wait at Sustain.
void ChaseDriver::BumpLevels( const ChaseDocument& doc, std::vector< float >& levels )
{
	const View& v       = doc.views[ Option( view, (int)doc.views.size() ) ];
	const Sequence& seq = doc.sequences[ Option( sequence, (int)doc.sequences.size() ) ];
	const int count     = (int)seq.steps.size();
	if( count <= 0 )
		return;
	const Envelope* env = ChosenEnvelope( doc, envelope );
	const int life      = 1 + std::clamp( (int)std::lround( echoes ), 0, MAX_ECHOES );
	const float length  = std::max( BumpLength(), 0.001f );
	const bool holding  = bumpHold > 0.5f;
	const float hold    = HoldPoint() * (float)life;
	const double now    = Now();

	std::lock_guard< std::mutex > lock( mutex );

	// Blast runs the sequence on by itself; otherwise, after a quiet spell, Auto Reset puts the
	// tapping back to the start so the same phrase can be played again.
	if( blasting )
	{
		const float speed = std::clamp( blastSpeed, 1.f, 50.f );
		if( blastNext <= 0.0 )
			blastNext = now;
		for( int fired = 0; now >= blastNext && fired < count; ++fired )
		{
			cursor = cursor < 0 ? 0 : ( cursor + 1 ) % count;
			StartLocked( cursor );
			blastNext += 1.0 / (double)speed;
			padsDirty = true;
		}
		lastTap = now;
	}
	else if( bumpAutoReset > 0.5f && cursor >= 0 && now - lastTap > std::max( bumpResetDelay, 0.1f ) )
		cursor = -1;

	for( size_t i = 0; i < bumps.size(); )
	{
		const Bump& b = bumps[ i ];
		// How far the bump has travelled, counted in steps of its own life.
		float t = (float)( ( now - b.start ) / length );
		if( holding )
		{
			if( b.held )
				t = std::min( t, hold );
			else
				t = std::min( (float)( ( b.released - b.start ) / length ), hold ) + (float)( ( now - b.released ) / length );
		}
		if( t >= (float)life )
		{
			bumps.erase( bumps.begin() + (long)i );
			padsDirty = true;
			continue;
		}
		for( int age = 0; age < life; ++age )
		{
			const int index     = ( ( b.step - age ) % count + count ) % count;
			const float x       = ( t + (float)age ) / (float)life;
			const float opacity = env ? SampleLut( env->lut, x ) : SustainRelease( sustain, std::max( release, sustain ), x );
			if( opacity <= 0.f )
				continue;
			for( int slice : seq.steps[ index ] )
				if( v.includes[ slice ] )
					levels[ slice ] = std::max( levels[ slice ], opacity );
		}
		++i;
	}
}

// Random picks a handful of the view's slices Random Speed times a second and lets each fade on
// its own curve. Echoes keeps a pick alive for that many picks longer, so the last few linger.
void ChaseDriver::RandomLevels( const ChaseDocument& doc, std::vector< float >& levels )
{
	const View& v = doc.views[ Option( view, (int)doc.views.size() ) ];
	std::vector< int > pool;
	for( size_t i = 0; i < doc.slices.size() && i < v.includes.size(); ++i )
		if( v.includes[ i ] )
			pool.push_back( (int)i );
	if( pool.empty() )
		return;

	const Envelope* env = ChosenEnvelope( doc, envelope );
	const int life      = 1 + std::clamp( (int)std::lround( echoes ), 0, MAX_ECHOES );
	const double slot   = 1.0 / (double)std::clamp( randomSpeed, 0.1f, 30.f );
	// Never ask for more slices than the view holds.
	const int most   = std::min( std::max( 1, (int)std::lround( randomMax ) ), (int)pool.size() );
	const double now = Now();

	std::lock_guard< std::mutex > lock( mutex );
	if( randomNext <= 0.0 )
		randomNext = now;
	// Catch up at most one slot's worth, so a stalled frame does not fire a burst.
	if( now - randomNext > slot )
		randomNext = now;
	while( now >= randomNext )
	{
		const int count = 1 + (int)( rng() % (unsigned int)most );
		// Shuffle just the front of the pool: enough for this pick, and cheap.
		for( int i = 0; i < count; ++i )
		{
			std::swap( pool[ i ], pool[ i + (int)( rng() % (unsigned int)( pool.size() - i ) ) ] );
			bumps.push_back( { pool[ i ], now, false, 0.0 } );
		}
		randomNext += slot;
	}

	for( size_t i = 0; i < bumps.size(); )
	{
		const float x = (float)( ( now - bumps[ i ].start ) / ( slot * life ) );
		if( x >= 1.f )
		{
			bumps.erase( bumps.begin() + (long)i );
			continue;
		}
		const float opacity = env ? SampleLut( env->lut, x ) : SustainRelease( sustain, std::max( release, sustain ), x );
		const int slice     = bumps[ i ].step;
		if( opacity > 0.f && slice >= 0 && slice < (int)levels.size() )
			levels[ slice ] = std::max( levels[ slice ], opacity );
		++i;
	}
}
}// namespace stilletchaser
