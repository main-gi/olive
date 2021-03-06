#ifndef CHROMAKEYEFFECT_H
#define CHROMAKEYEFFECT_H

#include "effects/effect.h"

class ChromaKeyEffect : public Effect {
public:
	ChromaKeyEffect(Clip* c);
	void process_shader(double timecode);
private:
	EffectField* color_field;
	EffectField* tolerance_field;
};

#endif // CHROMAKEYEFFECT_H
