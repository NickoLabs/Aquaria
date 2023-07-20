#include "TileRender.h"
#include "RenderBase.h"
#include "Core.h"
#include "Tileset.h"
#include "RenderGrid.h"
#include "RenderObject.h"


TileRender::TileRender(const TileStorage& tiles)
	: storage(tiles), renderBorders(false)
{
	this->cull = false;
	this->neverFollowCamera = true;
}

TileRender::~TileRender()
{
}

// shamelessly ripped from paint.net default palette
static const Vector s_tagColors[] =
{
	/* 0 */ Vector(0.5f, 0.5f, 0.5f),
	/* 1 */ Vector(1,0,0),
	/* 2 */ Vector(1, 0.415686f, 0),
	/* 3 */ Vector(1,0.847059f, 0),
	/* 4 */ Vector(0.298039f,1,0),
	/* 5 */ Vector(0,1,1),
	/* 6 */ Vector(0,0.580392,1),
	/* 7 */ Vector(0,0.149020f,1),
	/* 8 */ Vector(0.282353f,0,1),
	/* 9 */ Vector(0.698039f,0,1),

	/* 10 */ Vector(1,0,1), // anything outside of the pretty range
};

static inline const Vector& getTagColor(int tag)
{
	const unsigned idx = std::min<unsigned>(unsigned(tag), Countof(s_tagColors)-1);
	return s_tagColors[idx];

}

void TileRender::onRender(const RenderState& rs) const
{
	if(storage.tiles.empty())
		return;

	RenderState rx(rs);

	// prepare. get parallax scroll factors
	const RenderObjectLayer& rl = core->renderObjectLayers[this->layer];
	const Vector M = rl.followCameraMult; // affected by parallaxLock
	const float F = rl.followCamera;
	const bool parallax = rl.followCamera > 0;

	// Formula from RenderObject::getFollowCameraPosition() and optimized for speed
	const Vector C = core->screenCenter;
	const Vector M1 = Vector(1,1) - M;
	const Vector T = C * (1 - F);

	unsigned lastTexRepeat = false;
	unsigned lastTexId = 0;

	const bool renderExtras = renderBorders || RenderObject::renderCollisionShape;
	const TileEffectData *prevEff = ((TileEffectData*)NULL)+1; // initial value is different from anything else
	const RenderGrid *grid = NULL;
	const DynamicGPUBuffer *lastVertexBuf = NULL;

	for(size_t i = 0; i < storage.tiles.size(); ++i)
	{
		const TileData& tile = storage.tiles[i];
		if(tile.flags & (TILEFLAG_HIDDEN | TILEFLAG_EDITOR_HIDDEN))
			continue;

		Vector pos(tile.x, tile.y);
		if(parallax)
		{
			const Vector tmp = T + (F * pos);
			pos = pos * M1 + (tmp * M); // lerp, used to select whether to use original v or parallax-corrected v
		}

		const ElementTemplate * const et = tile.et;
		const float sw = et->w * tile.scalex;
		const float sh = et->h * tile.scaley;

		// adapted from RenderObject::isOnScreen()
		{
			const float cullRadiusSqr = ((sw*sw + sh*sh) * core->invGlobalScaleSqr) + core->cullRadiusSqr;
			if ((pos - core->cullCenter).getSquaredLength2D() >= cullRadiusSqr)
				continue;
		}

		if(const Texture * const tex = et->tex.content())
		{
			unsigned texid = tex->gltexid;
			unsigned rep = tile.flags & TILEFLAG_REPEAT;
			if(texid != lastTexId || rep != lastTexRepeat)
			{
				lastTexId = texid;
				lastTexRepeat = rep;
				tex->apply(!!rep);
			}
		}
		else
		{
			lastTexId = 0;
			glBindTexture(GL_TEXTURE_2D, 0); // unlikely
		}

		float alpha = rs.alpha;
		const TileEffectData * const eff = tile.eff;
		if(eff != prevEff) // effects between tiles are often shared so this works not only for NULL
		{
			prevEff = eff;
			BlendType blend = BLEND_DEFAULT;
			alpha = rs.alpha;
			grid = NULL;

			if(eff)
			{
				grid = eff->grid;
				alpha *= eff->alpha.x;
				blend = eff->blend;
			}

			rs.gpu.setBlend(blend);
			glColor4f(rs.color.x, rs.color.y, rs.color.z, alpha);
		}

		glPushMatrix();
		glTranslatef(pos.x, pos.y, pos.z);

		// HACK: Due to a renderer bug in older versions, vertical flip is ignored
		// when a grid-based tile effect is applied.
		// Maps were designed with the bug present so we need to replicate it,
		// otherwise things won't look correct.
		unsigned effflag = tile.flags;
		if(grid)
			effflag &= ~TILEFLAG_FV;

		float effrot = tile.rotation;

		// both set? that's effectively a rotation by 180�
		if ((effflag & (TILEFLAG_FH | TILEFLAG_FV)) == (TILEFLAG_FH | TILEFLAG_FV))
			effrot += 180;

		glRotatef(effrot, 0, 0, 1);

		switch(effflag & (TILEFLAG_FH | TILEFLAG_FV))
		{
			case TILEFLAG_FH:
				glRotatef(180, 0, 1, 0);
				break;

			case TILEFLAG_FV:
				glRotatef(180, 1, 0, 0);
				break;

			default: ; // both or none set, nothing to do
		}

		// this is only relevant in editor mode and is always 0 otherwise
		//glTranslatef(tile.beforeScaleOffsetX, tile.beforeScaleOffsetY, 0);

		glScalef(sw, sh, 1);


		if(!grid)
		{
			const DynamicGPUBuffer *vb = !(tile.flags & TILEFLAG_REPEAT)
				? tile.et->vertexbuf
				: &tile.rep->vertexbuf;
			assert(vb);
			if(vb != lastVertexBuf)
			{
				lastVertexBuf = vb;
				vb->apply();
			}
			vb->DrawArrays(GL_TRIANGLE_FAN, 4);
		}
		else
		{
			rx.alpha = alpha;

			Vector upperLeftTextureCoordinates, lowerRightTextureCoordinates;
			if(tile.flags & TILEFLAG_REPEAT)
			{
				upperLeftTextureCoordinates = Vector(tile.rep->tu1, tile.rep->tv1);
				lowerRightTextureCoordinates = Vector(tile.rep->tu2, tile.rep->tv2);
			}
			else
			{
				upperLeftTextureCoordinates = Vector(et->tu1, et->tv1);
				lowerRightTextureCoordinates = Vector(et->tu2, et->tv2);
			}

			grid->render(rx, upperLeftTextureCoordinates, lowerRightTextureCoordinates);
		}

		if(renderExtras)
		{
			glBindTexture(GL_TEXTURE_2D, 0);
			lastTexId = 0;
			prevEff = ((TileEffectData*)NULL)+1;

			if(grid && RenderObject::renderCollisionShape)
			{
				grid->renderDebugPoints(rs);
			}

			if(renderBorders)
			{
				float c = (tile.flags & TILEFLAG_SELECTED) ? 1.0f : 0.5f;
				Vector color(c,c,c);
				color *= getTagColor(tile.tag);

				glColor4f(color.x, color.y, color.z, 1.0f);
				glPointSize(16);
				glBegin(GL_POINTS);
					glVertex2f(0,0);
				glEnd();

				glLineWidth(2);
				glBegin(GL_LINE_STRIP);
					glVertex2f(0.5f, 0.5f);
					glVertex2f(0.5f, -0.5f);
					glVertex2f(-0.5f, -0.5f);
					glVertex2f(-0.5f, 0.5f);
					glVertex2f(0.5f, 0.5f);
				glEnd();
			}
		}

		glPopMatrix();
	}

	glBindBufferARB(GL_ARRAY_BUFFER, 0);

	RenderObject::lastTextureApplied = lastTexId;
	RenderObject::lastTextureRepeat = !!lastTexRepeat;
}

void TileRender::onUpdate(float dt)
{
	//this->position = core->screenCenter;
}
