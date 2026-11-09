#pragma once
#include <FFGLSDK.h>
#include <chrono>
#include <memory>
#include <vector>
#include "ChaseDocument.h"
#include "ChaseDriver.h"
#include "FrameHistory.h"
#include "Outline.h"
#include "PluginCommon.h"

// The all-in-one Stilletchaser effect.
// Driver decides which slices are lit (Chaser: a sequence stepped by the Step parameter,
// which Resolume animates with BPM sync, the timeline or FFT; Bumper: steps fired by hand from
// the Bump buttons, each one playing the opacity curve over Bump Length; Random: slices of the
// view picked at random, Random Speed times a second; None: every slice of the view).
// Modules add to it; the Delayer shows each slice's content some time ago, and the Wiper
// sends gradient wipes through the slices of a second sequence, and the Outliner draws slice
// outlines on top. Chase and wipes are combined (a slice shows wherever either lights it), so
// everything works together on the composition.
class Stilletchaser : public stilletchaser::StilletchaserPlugin
{
public:
	Stilletchaser();

	FFResult InitGL( const FFGLViewportStruct* vp ) override;
	FFResult ProcessOpenGL( ProcessOpenGLStruct* pGL ) override;
	FFResult DeInitGL() override;

	FFResult SetFloatParameter( unsigned int index, float value ) override;
	float GetFloatParameter( unsigned int index ) override;
	char* GetParameterDisplay( unsigned int index ) override;

	const char* GetShortName() override
	{
		static const char* sname = "StChase";
		return sname;
	}

private:
	friend struct StilletchaserTest;// plugin/tests/bumper_test.cpp

	// Every slice's triangles, and where its content comes from (the input or a delayed frame).
	struct SliceDraw
	{
		GLint first;
		GLsizei count;
		GLuint texture;
		bool fromHistory;
	};

	void RefreshMenus( bool raiseEvents );
	void UpdateVisibility( bool raiseEvents );
	// The sequence buttons: shown for the document's sequences, named after them, the chosen one marked.
	void UpdateSequenceButtons( bool raiseEvents );
	// The bump buttons: one per step of the chosen sequence, the ones still sounding marked.
	void UpdateBumpButtons( bool raiseEvents );
	// How long ago each slice's content comes from: frames with the Frames unit, seconds otherwise.
	void ComputeDelays( const stilletchaser::ChaseDocument& doc, std::vector< float >& delays ) const;
	bool DelayInFrames() const;
	// The full delay (Delay = 100%), in frames or seconds like ComputeDelays.
	float MaxDelay() const;
	void UpdateHistory( const FFGLTextureStruct& input, GLuint hostFBO );
	void BuildVertices( const stilletchaser::ChaseDocument& doc, const std::vector< float >& delays, const FFGLTextureStruct& input );
	std::vector< stilletchaser::ActiveWipe > CurrentWipes( const stilletchaser::ChaseDocument& doc ) const;
	void DrawWipes( const stilletchaser::ChaseDocument& doc, const FFGLTexCoords& inputMax );
	void DrawOutlines( const stilletchaser::ChaseDocument& doc, const std::vector< float >& levels );
	void BindSliceTexture( const SliceDraw& draw, const FFGLTexCoords& inputMax );
	float WipeParam( unsigned int index ) const;
	float OutlineParam( unsigned int index ) const;

	ffglex::FFGLShader shader;
	GLuint vao = 0, vbo = 0;
	std::vector< float > vertices;
	std::vector< SliceDraw > draws;
	stilletchaser::FrameHistory history;
	stilletchaser::OutlineRenderer outlines;

	std::shared_ptr< const stilletchaser::ChaseDocument > doc;
	uint64_t docVersion = 0;

	// Timing, for the Delayer's seconds and beats and its auto reset.
	std::chrono::steady_clock::time_point lastFrame;
	bool hasLastFrame     = false;
	float framesPerSecond = 60.f;
	double frameTime      = 0.0;// Seconds; when the current frame arrived.

	// Everything about which slices are lit and how brightly.
	stilletchaser::ChaseDriver chase;
	float scaling = 1.f;

	// Delayer
	float delayer       = 0.f;
	float delaySequence = 0.f;
	float delay         = 1.f;
	float delayUnit     = 2.f;
	float maxFrames     = 30.f;
	float maxSeconds    = 1.f;
	float maxBeats      = 2.f;
	float quality       = 0.75f;
	float reverse       = 0.f;
	float autoReset     = 1.f;
	float resetMinutes  = 5.f;

	// Wiper module: on/off, sequence, step, length, overlap, loops, flip, odd/even, swap, envelope,
	// source (video or solid colour) and the colour.
	static const int WIPE_PARAMS = 14;
	float wipe[ WIPE_PARAMS ] = { 0.f, 0.f, 0.f, 0.25f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 0.f, 1.f, 1.f, 1.f };

	// Outliner module: on/off, target (all or lit slices), outlines only, colour, width, feather,
	// animate, phase, fan, randomise, length, count, flip, odd/even, envelope.
	static const int OUTLINE_PARAMS = 17;
	float outline[ OUTLINE_PARAMS ] = { 0.f, 1.f, 0.f, 1.f, 1.f, 1.f, 10.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.25f, 1.f, 0.f, 0.f, 0.f };
};
