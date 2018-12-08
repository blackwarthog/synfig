/* === S Y N F I G ========================================================= */
/*!	\file warp.cpp
**	\brief Implementation of the "Warp" layer
**
**	$Id$
**
**	\legal
**	Copyright (c) 2002-2005 Robert B. Quattlebaum Jr., Adrian Bentley
**	Copyright (c) 2007, 2008 Chris Moore
**	Copyright (c) 2011-2013 Carlos López
**
**	This package is free software; you can redistribute it and/or
**	modify it under the terms of the GNU General Public License as
**	published by the Free Software Foundation; either version 2 of
**	the License, or (at your option) any later version.
**
**	This package is distributed in the hope that it will be useful,
**	but WITHOUT ANY WARRANTY; without even the implied warranty of
**	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
**	General Public License for more details.
**	\endlegal
**
** === N O T E S ===========================================================
**
** ========================================================================= */

/* === H E A D E R S ======================================================= */

#ifdef USING_PCH
#	include "pch.h"
#else
#ifdef HAVE_CONFIG_H
#	include <config.h>
#endif

#include <ETL/misc>

#include <synfig/string.h>
#include <synfig/time.h>
#include <synfig/context.h>
#include <synfig/paramdesc.h>
#include <synfig/renddesc.h>
#include <synfig/surface.h>
#include <synfig/value.h>
#include <synfig/valuenode.h>
#include <synfig/transform.h>
#include <synfig/cairo_renddesc.h>

#include <synfig/rendering/common/task/tasktransformation.h>
#include <synfig/rendering/common/task/taskblend.h>
#include <synfig/rendering/software/task/tasksw.h>

#include "warp.h"

#include <synfig/localization.h>
#include <synfig/general.h>

#endif

using namespace synfig;
using namespace modules;
using namespace lyr_std;

/* === M A C R O S ========================================================= */

/* === G L O B A L S ======================================================= */

SYNFIG_LAYER_INIT(Warp);
SYNFIG_LAYER_SET_NAME(Warp,"warp");
SYNFIG_LAYER_SET_LOCAL_NAME(Warp,N_("Warp"));
SYNFIG_LAYER_SET_CATEGORY(Warp,N_("Distortions"));
SYNFIG_LAYER_SET_VERSION(Warp,"0.1");
SYNFIG_LAYER_SET_CVS_ID(Warp,"$Id$");

/* === P R O C E D U R E S ================================================= */

namespace {

class TransformationPerspective: public rendering::Transformation
{
public:
	typedef etl::handle<TransformationPerspective> Handle;

	Matrix matrix;

	TransformationPerspective() { }
	explicit TransformationPerspective(const Matrix &matrix): matrix(matrix) { }

protected:
	virtual Transformation* clone_vfunc() const
		{ return new TransformationPerspective(matrix); }
	virtual Transformation* create_inverted_vfunc() const
		{ return new TransformationPerspective(Matrix(matrix).invert()); }

	virtual Point transform_vfunc(const Point &x, bool translate) const {
		Point p;
		Real z;
		matrix.get_transformed(p[0], p[1], z, x[0], x[1], translate ? 1.0 : 0.0);

	}

	virtual Bounds transform_bounds_vfunc(const Bounds &bounds) const;
	virtual bool can_merge_outer_vfunc(const Transformation &other) const;
	virtual bool can_merge_inner_vfunc(const Transformation &other) const;
	virtual void merge_outer_vfunc(const Transformation &other);
	virtual void merge_inner_vfunc(const Transformation &other);
};

class TaskTransformationPerspective: public rendering::TaskTransformation
{
public:
	typedef etl::handle<TaskTransformationPerspective> Handle;
	static Token token;
	virtual Token::Handle get_token() const { return token.handle(); }

	Holder<TransformationAffine> transformation;

	virtual const Transformation::Handle get_transformation() const
		{ return transformation.handle(); }

	virtual int get_pass_subtask_index() const;
};

class TaskPerspective: public rendering::Task, public rendering::TaskInterfaceTransformation
{
public:
	typedef etl::handle<TaskCheckerBoard> Handle;
	static Token token;
	virtual Token::Handle get_token() const { return token.handle(); }

	Color color;
	bool antialias;
	rendering::Holder<rendering::TransformationAffine> transformation;

	TaskCheckerBoard(): antialias(true) { }
	virtual const rendering::Transformation::Handle get_transformation() const
		{ return transformation.handle(); }
};


class TaskCheckerBoardSW: public TaskCheckerBoard, public rendering::TaskSW,
	public rendering::TaskInterfaceBlendToTarget,
	public rendering::TaskInterfaceSplit
{
public:
	typedef etl::handle<TaskCheckerBoardSW> Handle;
	static Token token;
	virtual Token::Handle get_token() const { return token.handle(); }

	virtual void on_target_set_as_source() {
		Task::Handle &subtask = sub_task(0);
		if ( subtask
		  && subtask->target_surface == target_surface
		  && !Color::is_straight(blend_method) )
		{
			trunc_by_bounds();
			subtask->source_rect = source_rect;
			subtask->target_rect = target_rect;
		}
	}

	virtual Color::BlendMethodFlags get_supported_blend_methods() const
		{ return Color::BLEND_METHODS_ALL; }

	virtual bool run(RunParams&) const {
		if (!is_valid())
			return true;

		Vector ppu = get_pixels_per_unit();

		Matrix bounds_transfromation;
		bounds_transfromation.m00 = ppu[0];
		bounds_transfromation.m11 = ppu[1];
		bounds_transfromation.m20 = target_rect.minx - ppu[0]*source_rect.minx;
		bounds_transfromation.m21 = target_rect.miny - ppu[1]*source_rect.miny;

		Matrix matrix = bounds_transfromation * transformation->matrix;
		Matrix inv_matrix = matrix.get_inverted();

		int tw = target_rect.get_width();
		Vector dx = inv_matrix.axis_x();
		Vector dy = inv_matrix.axis_y() - dx*(Real)tw;
		Vector p = inv_matrix.get_transformed( Vector((Real)target_rect.minx, (Real)target_rect.miny) );

		LockWrite la(this);
		if (!la)
			return false;

		Surface::alpha_pen apen(la->get_surface().get_pen(target_rect.minx, target_rect.miny));
		ColorReal amount = blend ? this->amount : ColorReal(1.0);
		apen.set_blend_method(blend ? blend_method : Color::BLEND_COMPOSITE);
		Color c = color;
		if (antialias) {
			ColorReal kx(matrix.axis_x().mag()*0.5);
			ColorReal ky(matrix.axis_y().mag()*0.5);
			for(int iy = target_rect.miny; iy < target_rect.maxy; ++iy, p += dy, apen.inc_y(), apen.dec_x(tw))
				for(int ix = target_rect.minx; ix < target_rect.maxx; ++ix, p += dx, apen.inc_x()) {
					p[0] -= floor(p[0]);
					p[1] -= floor(p[1]);

					ColorReal px = p[0]*ColorReal(2);
					px -= floor(px);
					px = std::min(px, ColorReal(1) - px)*kx;

					ColorReal py = p[1]*ColorReal(2);
					py -= floor(py);
					py = std::min(py, ColorReal(1) - py)*ky;

					ColorReal a = std::min(px, py);
					if ((p[0] < 0.5) != (p[1] < 0.5)) a = -a;
					a = std::max(ColorReal(0), std::min(ColorReal(1), a + ColorReal(0.5)));

					c.set_a(color.get_a()*a);
					apen.put_value(c, amount);
				}
		} else {
			for(int iy = target_rect.miny; iy < target_rect.maxy; ++iy, p += dy, apen.inc_y(), apen.dec_x(tw))
				for(int ix = target_rect.minx; ix < target_rect.maxx; ++ix, p += dx, apen.inc_x()) {
					p[0] -= floor(p[0]);
					p[1] -= floor(p[1]);
					ColorReal a((p[0] < 0.5) == (p[1] < 0.5) ? 1.0 : 0.0);
					c.set_a(color.get_a()*a);
					apen.put_value(c, amount);
				}
		}

		return true;
	}
};

rendering::Task::Token TaskCheckerBoard::token(
	DescAbstract<TaskCheckerBoard>("CheckerBoard") );
rendering::Task::Token TaskCheckerBoardSW::token(
	DescReal<TaskCheckerBoardSW, TaskCheckerBoard>("CheckerBoardSW") );

} // namespace

/* === M E T H O D S ======================================================= */

/* === E N T R Y P O I N T ================================================= */

Warp::Warp():
	param_src_tl  (ValueBase(Point(-2,2))),
	param_src_br  (ValueBase(Point(2,-2))),
	param_dest_tl (ValueBase(Point(-1.8,2.1))),
	param_dest_tr (ValueBase(Point(1.8,2.1))),
	param_dest_bl (ValueBase(Point(-2.2,-2))),
	param_dest_br (ValueBase(Point(2.2,-2))),
	param_clip	  (ValueBase(true))
{
	param_horizon=ValueBase(Real(4));
	sync();
	SET_INTERPOLATION_DEFAULTS();
	SET_STATIC_DEFAULTS();
}

Warp::~Warp()
{
}

inline Point
Warp::transform_forward(const Point& p)const
{
	return Point(
		(inv_matrix[0][0]*p[0] + inv_matrix[0][1]*p[1] + inv_matrix[0][2])/(inv_matrix[2][0]*p[0] + inv_matrix[2][1]*p[1] + inv_matrix[2][2]),
		(inv_matrix[1][0]*p[0] + inv_matrix[1][1]*p[1] + inv_matrix[1][2])/(inv_matrix[2][0]*p[0] + inv_matrix[2][1]*p[1] + inv_matrix[2][2])
	);
}

inline Point
Warp::transform_backward(const Point& p)const
{
	return Point(
		(matrix[0][0]*p[0] + matrix[0][1]*p[1] + matrix[0][2])/(matrix[2][0]*p[0] + matrix[2][1]*p[1] + matrix[2][2]),
		(matrix[1][0]*p[0] + matrix[1][1]*p[1] + matrix[1][2])/(matrix[2][0]*p[0] + matrix[2][1]*p[1] + matrix[2][2])
	);
}

inline Real
Warp::transform_forward_z(const Point& p)const
{
	return inv_matrix[2][0]*p[0] + inv_matrix[2][1]*p[1] + inv_matrix[2][2];
}

inline Real
Warp::transform_backward_z(const Point& p)const
{
	return matrix[2][0]*p[0] + matrix[2][1]*p[1] + matrix[2][2];
}

/*
#define transform_forward(p) Point(	\
		cache_a*p[0] + cache_b*p[1] + cache_c*p[0]*p[1] + cache_d,	\
		cache_e*p[0] + cache_f*p[1] + cache_i*p[0]*p[1] + cache_j )

#define transform_backward(p) Point(	\
		cache_a*p[0] + cache_b*p[1] + cache_c*p[0]*p[1] + cache_d,	\
		cache_e*p[0] + cache_f*p[1] + cache_i*p[0]*p[1] + cache_j )
*/

#define triangle_area(a,b,c)	(0.5*(-b[0]*a[1]+c[0]*a[1]+a[0]*b[1]-c[0]*b[1]-a[0]*c[1]+b[0]*c[1]))
#define quad_area(a,b,c,d) (triangle_area(a,b,c)+triangle_area(a,c,d))

Real mat3_determinant(Real matrix[3][3])
{
  Real ret;

  ret  = (matrix[0][0] *
                  (matrix[1][1] * matrix[2][2] -
                   matrix[1][2] * matrix[2][1]));
  ret -= (matrix[1][0] *
                  (matrix[0][1] * matrix[2][2] -
                   matrix[0][2] * matrix[2][1]));
  ret += (matrix[2][0] *
                  (matrix[0][1] * matrix[1][2] -
                   matrix[0][2] * matrix[1][1]));

return ret;
}

void mat3_invert(Real in[3][3], Real out[3][3])
{
  Real det(mat3_determinant(in));

	if (det == 0.0)
    return;

  det = 1.0 / det;

  out[0][0] =   (in[1][1] * in[2][2] -
                       in[1][2] * in[2][1]) * det;

  out[1][0] = - (in[1][0] * in[2][2] -
                       in[1][2] * in[2][0]) * det;

  out[2][0] =   (in[1][0] * in[2][1] -
                       in[1][1] * in[2][0]) * det;

  out[0][1] = - (in[0][1] * in[2][2] -
                       in[0][2] * in[2][1]) * det;

  out[1][1] =   (in[0][0] * in[2][2] -
                       in[0][2] * in[2][0]) * det;

  out[2][1] = - (in[0][0] * in[2][1] -
                       in[0][1] * in[2][0]) * det;

  out[0][2] =   (in[0][1] * in[1][2] -
                       in[0][2] * in[1][1]) * det;

  out[1][2] = - (in[0][0] * in[1][2] -
                       in[0][2] * in[1][0]) * det;

  out[2][2] =   (in[0][0] * in[1][1] -
                       in[0][1] * in[1][0]) * det;

}

void
Warp::sync()
{
/*	cache_a=(-dest_tl[0]+dest_tr[0])/(src_br[1]-src_tl[1]);
	cache_b=(-dest_tl[0]+dest_bl[0])/(src_br[0]-src_tl[0]);
	cache_c=(dest_tl[0]-dest_tr[0]+dest_br[0]-dest_bl[0])/((src_br[1]-src_tl[1])*(src_br[0]-src_tl[0]));
	cache_d=dest_tl[0];

	cache_e=(-dest_tl[1]+dest_tr[1])/(src_br[0]-src_tl[0]);
	cache_f=(-dest_tl[1]+dest_bl[1])/(src_br[1]-src_tl[1]);
	cache_i=(dest_tl[1]-dest_tr[1]+dest_br[1]-dest_bl[1])/((src_br[1]-src_tl[1])*(src_br[0]-src_tl[0]));
	cache_j=dest_tl[1];
*/

/*	matrix[2][0]=(dest_tl[0]-dest_tr[0]+dest_br[0]-dest_bl[0])/((src_br[1]-src_tl[1])*(src_br[0]-src_tl[0]));
	matrix[2][1]=(dest_tl[1]-dest_tr[1]+dest_br[1]-dest_bl[1])/((src_br[1]-src_tl[1])*(src_br[0]-src_tl[0]));
	matrix[2][2]=quad_area(dest_tl,dest_tr,dest_br,dest_bl)/((src_br[1]-src_tl[1])*(src_br[0]-src_tl[0]));

	matrix[0][0]=-(-dest_tl[1]+dest_tr[1])/(src_br[0]-src_tl[0]);
	matrix[0][1]=-(-dest_tl[1]+dest_bl[1])/(src_br[1]-src_tl[1]);

	matrix[1][0]=-(-dest_tl[0]+dest_tr[0])/(src_br[1]-src_tl[1]);
	matrix[1][1]=-(-dest_tl[0]+dest_bl[0])/(src_br[0]-src_tl[0]);

	matrix[0][2]=matrix[0][0]*dest_tl[0] + matrix[0][1]*dest_tl[1];
	matrix[1][2]=matrix[1][0]*dest_tl[0] + matrix[1][1]*dest_tl[1];
*/
	Point src_tl=param_src_tl.get(Point());
	Point src_br=param_src_br.get(Point());
	Point dest_tl=param_dest_tl.get(Point());
	Point dest_tr=param_dest_tr.get(Point());
	Point dest_bl=param_dest_bl.get(Point());
	Point dest_br=param_dest_br.get(Point());
#define matrix tmp
	Real tmp[3][3];

	const Real& x1(min(src_br[0],src_tl[0]));
	const Real& y1(min(src_br[1],src_tl[1]));
	const Real& x2(max(src_br[0],src_tl[0]));
	const Real& y2(max(src_br[1],src_tl[1]));

	Real tx1(dest_bl[0]);
	Real ty1(dest_bl[1]);
	Real tx2(dest_br[0]);
	Real ty2(dest_br[1]);
	Real tx3(dest_tl[0]);
	Real ty3(dest_tl[1]);
	Real tx4(dest_tr[0]);
	Real ty4(dest_tr[1]);

	if(src_br[0]<src_tl[0])
		swap(tx3,tx4),swap(ty3,ty4),swap(tx1,tx2),swap(ty1,ty2);

	if(src_br[1]>src_tl[1])
		swap(tx3,tx1),swap(ty3,ty1),swap(tx4,tx2),swap(ty4,ty2);

	Real scalex;
	Real scaley;

  scalex = scaley = 1.0;

  if ((x2 - x1) > 0)
    scalex = 1.0 / (Real) (x2 - x1);

  if ((y2 - y1) > 0)
    scaley = 1.0 / (Real) (y2 - y1);

  /* Determine the perspective transform that maps from
   * the unit cube to the transformed coordinates
   */
  {
    Real dx1, dx2, dx3, dy1, dy2, dy3;

    dx1 = tx2 - tx4;
    dx2 = tx3 - tx4;
    dx3 = tx1 - tx2 + tx4 - tx3;

    dy1 = ty2 - ty4;
    dy2 = ty3 - ty4;
    dy3 = ty1 - ty2 + ty4 - ty3;

    /*  Is the mapping affine?  */
    if ((dx3 == 0.0) && (dy3 == 0.0))
      {
        matrix[0][0] = tx2 - tx1;
        matrix[0][1] = tx4 - tx2;
        matrix[0][2] = tx1;
        matrix[1][0] = ty2 - ty1;
        matrix[1][1] = ty4 - ty2;
        matrix[1][2] = ty1;
        matrix[2][0] = 0.0;
        matrix[2][1] = 0.0;
      }
    else
      {
        Real det1, det2;

        det1 = dx3 * dy2 - dy3 * dx2;
        det2 = dx1 * dy2 - dy1 * dx2;

        if (det1 == 0.0 && det2 == 0.0)
          matrix[2][0] = 1.0;
        else
          matrix[2][0] = det1 / det2;

        det1 = dx1 * dy3 - dy1 * dx3;

        if (det1 == 0.0 && det2 == 0.0)
          matrix[2][1] = 1.0;
        else
          matrix[2][1] = det1 / det2;

        matrix[0][0] = tx2 - tx1 + matrix[2][0] * tx2;
        matrix[0][1] = tx3 - tx1 + matrix[2][1] * tx3;
        matrix[0][2] = tx1;

        matrix[1][0] = ty2 - ty1 + matrix[2][0] * ty2;
        matrix[1][1] = ty3 - ty1 + matrix[2][1] * ty3;
        matrix[1][2] = ty1;
      }

    matrix[2][2] = 1.0;
  }
#undef matrix

	Real scaletrans[3][3]={
			{ scalex, 0, -x1*scalex },
			{ 0, scaley, -y1*scaley },
			{ 0, 0, 1 }
	};

	Real t1,t2,t3;

	for (int i = 0; i < 3; i++)
    {
      t1 = tmp[i][0];
      t2 = tmp[i][1];
      t3 = tmp[i][2];

      for (int j = 0; j < 3; j++)
        {
          matrix[i][j]  = t1 * scaletrans[0][j];
          matrix[i][j] += t2 * scaletrans[1][j];
          matrix[i][j] += t3 * scaletrans[2][j];
        }
    }

	mat3_invert(matrix, inv_matrix);
/*
	gimp_matrix3_identity  (result);
  gimp_matrix3_translate (result, -x1, -y1);
  gimp_matrix3_scale     (result, scalex, scaley);
  gimp_matrix3_mult      (&matrix, result);
*/
}

bool
Warp::set_param(const String & param, const ValueBase &value)
{
	IMPORT_VALUE_PLUS(param_src_tl,sync());
	IMPORT_VALUE_PLUS(param_src_br,sync());
	IMPORT_VALUE_PLUS(param_dest_tl,sync());
	IMPORT_VALUE_PLUS(param_dest_tr,sync());
	IMPORT_VALUE_PLUS(param_dest_bl,sync());
	IMPORT_VALUE_PLUS(param_dest_br,sync());
	IMPORT_VALUE(param_clip);
	IMPORT_VALUE(param_horizon);

	return false;
}

ValueBase
Warp::get_param(const String &param)const
{
	EXPORT_VALUE(param_src_tl);
	EXPORT_VALUE(param_src_br);
	EXPORT_VALUE(param_dest_tl);
	EXPORT_VALUE(param_dest_tr);
	EXPORT_VALUE(param_dest_bl);
	EXPORT_VALUE(param_dest_br);
	EXPORT_VALUE(param_clip);
	EXPORT_VALUE(param_horizon);

	EXPORT_NAME();
	EXPORT_VERSION();

	return ValueBase();
}

Layer::Vocab
Warp::get_param_vocab()const
{
	Layer::Vocab ret;

	ret.push_back(ParamDesc("src_tl")
		.set_local_name(_("Source TL"))
		.set_box("src_br")
		.set_description(_("Top Left corner of the source to warp"))
	);

	ret.push_back(ParamDesc("src_br")
		.set_local_name(_("Source BR"))
		.set_description(_("Bottom Right corner of the source to warp"))
	);

	ret.push_back(ParamDesc("dest_tl")
		.set_local_name(_("Dest TL"))
		.set_connect("dest_tr")
		.set_description(_("Top Left corner of the destination"))
	);

	ret.push_back(ParamDesc("dest_tr")
		.set_local_name(_("Dest TR"))
		.set_connect("dest_br")
		.set_description(_("Top Right corner of the destination"))
	);

	ret.push_back(ParamDesc("dest_br")
		.set_local_name(_("Dest BR"))
		.set_connect("dest_bl")
		.set_description(_("Bottom Right corner of the destination"))
	);

	ret.push_back(ParamDesc("dest_bl")
		.set_local_name(_("Dest BL"))
		.set_connect("dest_tl")
		.set_description(_("Bottom Left corner of the destination"))
	);

	ret.push_back(ParamDesc("clip")
		.set_local_name(_("Clip"))
	);

	ret.push_back(ParamDesc("horizon")
		.set_local_name(_("Horizon"))
		.set_description(_("Height that determines the horizon in perspectives"))
	);

	return ret;
}


class lyr_std::Warp_Trans : public Transform
{
	etl::handle<const Warp> layer;
public:
	Warp_Trans(const Warp* x):Transform(x->get_guid()),layer(x) { }

	Vector perform(const Vector& x)const
	{
		return layer->transform_backward(x);
		//Point pos(x-layer->origin);
		//return Point(layer->cos_val*pos[0]-layer->sin_val*pos[1],layer->sin_val*pos[0]+layer->cos_val*pos[1])+layer->origin;
	}

	Vector unperform(const Vector& x)const
	{

		return layer->transform_forward(x);
		//Point pos(x-layer->origin);
		//return Point(layer->cos_val*pos[0]+layer->sin_val*pos[1],-layer->sin_val*pos[0]+layer->cos_val*pos[1])+layer->origin;
	}

	String get_string()const
	{
		return "warp";
	}
};
etl::handle<Transform>
Warp::get_transform()const
{
	return new Warp_Trans(this);
}

Layer::Handle
Warp::hit_check(Context context, const Point &p)const
{
	Point src_tl=param_src_tl.get(Point());
	Point src_br=param_src_br.get(Point());
	bool clip=param_clip.get(bool());
	
	Point newpos(transform_forward(p));

	if(clip)
	{
		Rect rect(src_tl,src_br);
		if(!rect.is_inside(newpos))
			return 0;
	}

	return context.hit_check(newpos);
}

Color
Warp::get_color(Context context, const Point &p)const
{
	Point src_tl=param_src_tl.get(Point());
	Point src_br=param_src_br.get(Point());
	Real horizon=param_horizon.get(Real());
	bool clip=param_clip.get(bool());

	Point newpos(transform_forward(p));

	if(clip)
	{
		Rect rect(src_tl,src_br);
		if(!rect.is_inside(newpos))
			return Color::alpha();
	}

	const float z(transform_backward_z(newpos));
	if(z>0 && z<horizon)
		return context.get_color(newpos);
	else
		return Color::alpha();
}

//#define ACCEL_WARP_IS_BROKEN 1

RendDesc
Warp::get_sub_renddesc_vfunc(const RendDesc &renddesc) const
{
	Point dest_tl=param_dest_tl.get(Point());
	Point dest_tr=param_dest_tr.get(Point());
	Point dest_bl=param_dest_bl.get(Point());
	Point dest_br=param_dest_br.get(Point());
	Real horizon=param_horizon.get(Real());
	bool clip=param_clip.get(bool());

	Point tl(renddesc.get_tl());
	Point br(renddesc.get_br());

	Rect bounding_rect;

	Rect render_rect(tl,br);
	Rect clip_rect(Rect::full_plane());
	Rect dest_rect(dest_tl,dest_br); dest_rect.expand(dest_tr).expand(dest_bl);

	Real zoom_factor(1.0);

	{
		Rect other(render_rect);
		if(clip)
			other&=dest_rect;

		Point min(other.get_min());
		Point max(other.get_max());

		bool init_point_set=false;

		// Point trans_point[4];
		Point p;
		// Real trans_z[4];
		Real z,minz(10000000000000.0f),maxz(0);

		//! \todo checking the 4 corners for 0<=z<horizon*2 and using
		//! only 4 corners which satisfy this condition isn't the
		//! right thing to do.  It's possible that none of the 4
		//! corners fall within that range, and yet content of the
		//! tile does.
		p=transform_forward(min);
		z=transform_backward_z(p);
		if(z>0 && z<horizon*2)
		{
			if(init_point_set)
				bounding_rect.expand(p);
			else
				bounding_rect=Rect(p);
			init_point_set=true;
			maxz=std::max(maxz,z);
			minz=std::min(minz,z);
		}

		p=transform_forward(max);
		z=transform_backward_z(p);
		if(z>0 && z<horizon*2)
		{
			if(init_point_set)
				bounding_rect.expand(p);
			else
				bounding_rect=Rect(p);
			init_point_set=true;
			maxz=std::max(maxz,z);
			minz=std::min(minz,z);
		}

		swap(min[1],max[1]);

		p=transform_forward(min);
		z=transform_backward_z(p);
		if(z>0 && z<horizon*2)
		{
			if(init_point_set)
				bounding_rect.expand(p);
			else
				bounding_rect=Rect(p);
			init_point_set=true;
			maxz=std::max(maxz,z);
			minz=std::min(minz,z);
		}

		p=transform_forward(max);
		z=transform_backward_z(p);
		if(z>0 && z<horizon*2)
		{
			if(init_point_set)
				bounding_rect.expand(p);
			else
				bounding_rect=Rect(p);
			init_point_set=true;
			maxz=std::max(maxz,z);
			minz=std::min(minz,z);
		}

		zoom_factor=(1+(maxz-minz));

	}

	Point min_point(bounding_rect.get_min());
	Point max_point(bounding_rect.get_max());

	// we're going to divide by the difference of these pairs soon;
	// if they're the same, we'll be dividing by zero, and we don't
	// want to do that!
	// \todo what should we do in this case?
	if (min_point[0] == max_point[0]) max_point[0] += 0.001;
	if (min_point[1] == max_point[1]) max_point[1] += 0.001;

	if(tl[0]>br[0])
	{
		tl[0]=max_point[0];
		br[0]=min_point[0];
	}
	else
	{
		br[0]=max_point[0];
		tl[0]=min_point[0];
	}
	if(tl[1]>br[1])
	{
		tl[1]=max_point[1];
		br[1]=min_point[1];
	}
	else
	{
		br[1]=max_point[1];
		tl[1]=min_point[1];
	}

	const int tmp_d(max(renddesc.get_w(),renddesc.get_h()));
	Real src_pw=(tmp_d*zoom_factor)/(br[0]-tl[0]);
	Real src_ph=(tmp_d*zoom_factor)/(br[1]-tl[1]);

	RendDesc desc(renddesc);
	desc.clear_flags();
	desc.set_tl(tl);
	desc.set_br(br);
	desc.set_wh(ceil_to_int(src_pw*(br[0]-tl[0])),ceil_to_int(src_ph*(br[1]-tl[1])));

	return desc;
}

bool
Warp::accelerated_render(Context context,Surface *surface,int quality, const RendDesc &renddesc, ProgressCallback *cb)const
{
	RENDER_TRANSFORMED_IF_NEED(__FILE__, __LINE__)

	Point src_tl=param_src_tl.get(Point());
	Point src_br=param_src_br.get(Point());
	Point dest_tl=param_dest_tl.get(Point());
	Point dest_tr=param_dest_tr.get(Point());
	Point dest_bl=param_dest_bl.get(Point());
	Point dest_br=param_dest_br.get(Point());
	Real horizon=param_horizon.get(Real());
	bool clip=param_clip.get(bool());

	SuperCallback stageone(cb,0,9000,10000);
	SuperCallback stagetwo(cb,9000,10000,10000);

	Real pw=(renddesc.get_w())/(renddesc.get_br()[0]-renddesc.get_tl()[0]);
	Real ph=(renddesc.get_h())/(renddesc.get_br()[1]-renddesc.get_tl()[1]);

	if(cb && !cb->amount_complete(0,10000))
		return false;

	Point tl(renddesc.get_tl());
	Point br(renddesc.get_br());

	Rect bounding_rect;

	Rect render_rect(tl,br);
	Rect clip_rect(Rect::full_plane());
	Rect dest_rect(dest_tl,dest_br); dest_rect.expand(dest_tr).expand(dest_bl);

	Real zoom_factor(1.0);

	// Quick exclusion clip, if necessary
	if(clip && !intersect(render_rect,dest_rect))
	{
		surface->set_wh(renddesc.get_w(),renddesc.get_h());
		surface->clear();
		return true;
	}

	{
		Rect other(render_rect);
		if(clip)
			other&=dest_rect;

		Point min(other.get_min());
		Point max(other.get_max());

		bool init_point_set=false;

		// Point trans_point[4];
		Point p;
		// Real trans_z[4];
		Real z,minz(10000000000000.0f),maxz(0);

		//! \todo checking the 4 corners for 0<=z<horizon*2 and using
		//! only 4 corners which satisfy this condition isn't the
		//! right thing to do.  It's possible that none of the 4
		//! corners fall within that range, and yet content of the
		//! tile does.
		p=transform_forward(min);
		z=transform_backward_z(p);
		if(z>0 && z<horizon*2)
		{
			if(init_point_set)
				bounding_rect.expand(p);
			else
				bounding_rect=Rect(p);
			init_point_set=true;
			maxz=std::max(maxz,z);
			minz=std::min(minz,z);
		}

		p=transform_forward(max);
		z=transform_backward_z(p);
		if(z>0 && z<horizon*2)
		{
			if(init_point_set)
				bounding_rect.expand(p);
			else
				bounding_rect=Rect(p);
			init_point_set=true;
			maxz=std::max(maxz,z);
			minz=std::min(minz,z);
		}

		swap(min[1],max[1]);

		p=transform_forward(min);
		z=transform_backward_z(p);
		if(z>0 && z<horizon*2)
		{
			if(init_point_set)
				bounding_rect.expand(p);
			else
				bounding_rect=Rect(p);
			init_point_set=true;
			maxz=std::max(maxz,z);
			minz=std::min(minz,z);
		}

		p=transform_forward(max);
		z=transform_backward_z(p);
		if(z>0 && z<horizon*2)
		{
			if(init_point_set)
				bounding_rect.expand(p);
			else
				bounding_rect=Rect(p);
			init_point_set=true;
			maxz=std::max(maxz,z);
			minz=std::min(minz,z);
		}

		if(!init_point_set)
		{
			surface->set_wh(renddesc.get_w(),renddesc.get_h());
			surface->clear();
			return true;
		}
		zoom_factor=(1+(maxz-minz));

	}

#ifdef ACCEL_WARP_IS_BROKEN
	return Layer::accelerated_render(context,surface,quality,renddesc, cb);
#else

	/*swap(tl[1],br[1]);
	bounding_rect
		.expand(transform_forward(tl))
		.expand(transform_forward(br))
	;
	swap(tl[1],br[1]);*/

	//warning("given window: [%f,%f]-[%f,%f] %dx%d",tl[0],tl[1],br[0],br[1],renddesc.get_w(),renddesc.get_h());
	//warning("Projected: [%f,%f]-[%f,%f]",bounding_rect.get_min()[0],bounding_rect.get_min()[1],bounding_rect.get_max()[0],bounding_rect.get_max()[1]);

	// If we are clipping, then go ahead and clip to the
	// source rectangle
	if(clip)
		clip_rect&=Rect(src_tl,src_br);

	// Bound ourselves to the bounding rectangle of
	// what is under us
	clip_rect&=context.get_full_bounding_rect();//.expand_x(abs(zoom_factor/pw)).expand_y(abs(zoom_factor/ph));

	bounding_rect&=clip_rect;

	Point min_point(bounding_rect.get_min());
	Point max_point(bounding_rect.get_max());

	// we're going to divide by the difference of these pairs soon;
	// if they're the same, we'll be dividing by zero, and we don't
	// want to do that!
	// \todo what should we do in this case?
	if (min_point[0] == max_point[0]) max_point[0] += 0.001;
	if (min_point[1] == max_point[1]) max_point[1] += 0.001;

	if(tl[0]>br[0])
	{
		tl[0]=max_point[0];
		br[0]=min_point[0];
	}
	else
	{
		br[0]=max_point[0];
		tl[0]=min_point[0];
	}
	if(tl[1]>br[1])
	{
		tl[1]=max_point[1];
		br[1]=min_point[1];
	}
	else
	{
		br[1]=max_point[1];
		tl[1]=min_point[1];
	}



	const int tmp_d(max(renddesc.get_w(),renddesc.get_h()));
	Real src_pw=(tmp_d*zoom_factor)/(br[0]-tl[0]);
	Real src_ph=(tmp_d*zoom_factor)/(br[1]-tl[1]);


	RendDesc desc(renddesc);
	desc.clear_flags();
	//desc.set_flags(RendDesc::PX_ASPECT);
	desc.set_tl(tl);
	desc.set_br(br);
	desc.set_wh(ceil_to_int(src_pw*(br[0]-tl[0])),ceil_to_int(src_ph*(br[1]-tl[1])));

	//warning("surface to render: [%f,%f]-[%f,%f] %dx%d",desc.get_tl()[0],desc.get_tl()[1],desc.get_br()[0],desc.get_br()[1],desc.get_w(),desc.get_h());
	if(desc.get_w()==0 && desc.get_h()==0)
	{
		surface->set_wh(renddesc.get_w(),renddesc.get_h());
		surface->clear();
		return true;
	}

	// Recalculate the pixel widths for the src renddesc
	src_pw=(desc.get_w())/(desc.get_br()[0]-desc.get_tl()[0]);
	src_ph=(desc.get_h())/(desc.get_br()[1]-desc.get_tl()[1]);


	Surface source;
	source.set_wh(desc.get_w(),desc.get_h());

	if(!context.accelerated_render(&source,quality,desc,&stageone))
		return false;

	surface->set_wh(renddesc.get_w(),renddesc.get_h());
	surface->clear();

	Surface::pen pen(surface->begin());

	if(quality<=4)
	{
		// CUBIC
		int x,y;
		float u,v;
		Point point,tmp;
		for(y=0,point[1]=renddesc.get_tl()[1];y<surface->get_h();y++,pen.inc_y(),pen.dec_x(x),point[1]+=1.0/ph)
		{
			for(x=0,point[0]=renddesc.get_tl()[0];x<surface->get_w();x++,pen.inc_x(),point[0]+=1.0/pw)
			{
				tmp=transform_forward(point);
				const float z(transform_backward_z(tmp));
				if(!clip_rect.is_inside(tmp) || !(z>0 && z<horizon))
				{
					(*surface)[y][x]=Color::alpha();
					continue;
				}

				u=(tmp[0]-tl[0])*src_pw;
				v=(tmp[1]-tl[1])*src_ph;

				if(u<0 || v<0 || u>=source.get_w() || v>=source.get_h() || std::isnan(u) || std::isnan(v))
					(*surface)[y][x]=context.get_color(tmp);
				else
					(*surface)[y][x]=source.cubic_sample(u,v);
			}
			if((y&31)==0 && cb)
			{
				if(!stagetwo.amount_complete(y,surface->get_h()))
					return false;
			}
		}
	}
	else
	if(quality<=6)
	{
		// INTERPOLATION_LINEAR
		int x,y;
		float u,v;
		Point point,tmp;
		for(y=0,point[1]=renddesc.get_tl()[1];y<surface->get_h();y++,pen.inc_y(),pen.dec_x(x),point[1]+=1.0/ph)
		{
			for(x=0,point[0]=renddesc.get_tl()[0];x<surface->get_w();x++,pen.inc_x(),point[0]+=1.0/pw)
			{
				tmp=transform_forward(point);
				const float z(transform_backward_z(tmp));
				if(!clip_rect.is_inside(tmp) || !(z>0 && z<horizon))
				{
					(*surface)[y][x]=Color::alpha();
					continue;
				}

				u=(tmp[0]-tl[0])*src_pw;
				v=(tmp[1]-tl[1])*src_ph;

				if(u<0 || v<0 || u>=source.get_w() || v>=source.get_h() || std::isnan(u) || std::isnan(v))
					(*surface)[y][x]=context.get_color(tmp);
				else
					(*surface)[y][x]=source.linear_sample(u,v);
			}
			if((y&31)==0 && cb)
			{
				if(!stagetwo.amount_complete(y,surface->get_h()))
					return false;
			}
		}
	}
	else
	{
		// NEAREST_NEIGHBOR
		int x,y;
		float u,v;
		Point point,tmp;
		for(y=0,point[1]=renddesc.get_tl()[1];y<surface->get_h();y++,pen.inc_y(),pen.dec_x(x),point[1]+=1.0/ph)
		{
			for(x=0,point[0]=renddesc.get_tl()[0];x<surface->get_w();x++,pen.inc_x(),point[0]+=1.0/pw)
			{
				tmp=transform_forward(point);
				const float z(transform_backward_z(tmp));
				if(!clip_rect.is_inside(tmp) || !(z>0 && z<horizon))
				{
					(*surface)[y][x]=Color::alpha();
					continue;
				}

				u=(tmp[0]-tl[0])*src_pw;
				v=(tmp[1]-tl[1])*src_ph;

				if(u<0 || v<0 || u>=source.get_w() || v>=source.get_h() || std::isnan(u) || std::isnan(v))
					(*surface)[y][x]=context.get_color(tmp);
				else
					//pen.set_value(source[v][u]);
					(*surface)[y][x]=source[floor_to_int(v)][floor_to_int(u)];
			}
			if((y&31)==0 && cb)
			{
				if(!stagetwo.amount_complete(y,surface->get_h()))
					return false;
			}
		}
	}

#endif

	if(cb && !cb->amount_complete(10000,10000)) return false;

	return true;
}


//////////
bool
Warp::accelerated_cairorender(Context context, cairo_t *cr, int quality, const RendDesc &renddesc_, ProgressCallback *cb)const
{
	Point src_tl=param_src_tl.get(Point());
	Point src_br=param_src_br.get(Point());
	Point dest_tl=param_dest_tl.get(Point());
	Point dest_tr=param_dest_tr.get(Point());
	Point dest_bl=param_dest_bl.get(Point());
	Point dest_br=param_dest_br.get(Point());
	Real horizon=param_horizon.get(Real());
	bool clip=param_clip.get(bool());

	SuperCallback stageone(cb,0,9000,10000);
	SuperCallback stagetwo(cb,9000,10000,10000);
	
	
	RendDesc renddesc(renddesc_);
	// Untransform the render desc
	if(!cairo_renddesc_untransform(cr, renddesc))
		return false;
	
	Real pw=(renddesc.get_w())/(renddesc.get_br()[0]-renddesc.get_tl()[0]);
	Real ph=(renddesc.get_h())/(renddesc.get_br()[1]-renddesc.get_tl()[1]);
	
	if(cb && !cb->amount_complete(0,10000))
		return false;
	
	Point tl(renddesc.get_tl());
	Point br(renddesc.get_br());
	
	Rect bounding_rect;
	
	Rect render_rect(tl,br);
	Rect clip_rect(Rect::full_plane());
	Rect dest_rect(dest_tl,dest_br); dest_rect.expand(dest_tr).expand(dest_bl);
	
	Real zoom_factor(1.0);
	
	// Quick exclusion clip, if necessary
	if(clip && !intersect(render_rect,dest_rect))
	{
		cairo_save(cr);
		cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
		cairo_paint(cr);
		cairo_restore(cr);
		return true;
	}
	
	{
		Rect other(render_rect);
		if(clip)
			other&=dest_rect;
		
		Point min(other.get_min());
		Point max(other.get_max());
		
		bool init_point_set=false;
		
		// Point trans_point[4];
		Point p;
		// Real trans_z[4];
		Real z,minz(10000000000000.0f),maxz(0);
		
		//! \todo checking the 4 corners for 0<=z<horizon*2 and using
		//! only 4 corners which satisfy this condition isn't the
		//! right thing to do.  It's possible that none of the 4
		//! corners fall within that range, and yet content of the
		//! tile does.
		p=transform_forward(min);
		z=transform_backward_z(p);
		if(z>0 && z<horizon*2)
		{
			if(init_point_set)
				bounding_rect.expand(p);
			else
				bounding_rect=Rect(p);
			init_point_set=true;
			maxz=std::max(maxz,z);
			minz=std::min(minz,z);
		}
		
		p=transform_forward(max);
		z=transform_backward_z(p);
		if(z>0 && z<horizon*2)
		{
			if(init_point_set)
				bounding_rect.expand(p);
			else
				bounding_rect=Rect(p);
			init_point_set=true;
			maxz=std::max(maxz,z);
			minz=std::min(minz,z);
		}
		
		swap(min[1],max[1]);
		
		p=transform_forward(min);
		z=transform_backward_z(p);
		if(z>0 && z<horizon*2)
		{
			if(init_point_set)
				bounding_rect.expand(p);
			else
				bounding_rect=Rect(p);
			init_point_set=true;
			maxz=std::max(maxz,z);
			minz=std::min(minz,z);
		}
		
		p=transform_forward(max);
		z=transform_backward_z(p);
		if(z>0 && z<horizon*2)
		{
			if(init_point_set)
				bounding_rect.expand(p);
			else
				bounding_rect=Rect(p);
			init_point_set=true;
			maxz=std::max(maxz,z);
			minz=std::min(minz,z);
		}
		
		if(!init_point_set)
		{
			cairo_save(cr);
			cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
			cairo_paint(cr);
			cairo_restore(cr);
			return true;
		}
		zoom_factor=(1+(maxz-minz));
		
	}
	
#ifdef ACCEL_WARP_IS_BROKEN
	return Layer::accelerated_cairorender(context,cr,quality,renddesc, cb);
#else
	
	/*swap(tl[1],br[1]);
	 bounding_rect
	 .expand(transform_forward(tl))
	 .expand(transform_forward(br))
	 ;
	 swap(tl[1],br[1]);*/
	
	//warning("given window: [%f,%f]-[%f,%f] %dx%d",tl[0],tl[1],br[0],br[1],renddesc.get_w(),renddesc.get_h());
	//warning("Projected: [%f,%f]-[%f,%f]",bounding_rect.get_min()[0],bounding_rect.get_min()[1],bounding_rect.get_max()[0],bounding_rect.get_max()[1]);
	
	// If we are clipping, then go ahead and clip to the
	// source rectangle
	if(clip)
		clip_rect&=Rect(src_tl,src_br);
	
	// Bound ourselves to the bounding rectangle of
	// what is under us
	clip_rect&=context.get_full_bounding_rect();//.expand_x(abs(zoom_factor/pw)).expand_y(abs(zoom_factor/ph));
	
	bounding_rect&=clip_rect;
	
	Point min_point(bounding_rect.get_min());
	Point max_point(bounding_rect.get_max());
	
	// we're going to divide by the difference of these pairs soon;
	// if they're the same, we'll be dividing by zero, and we don't
	// want to do that!
	// \todo what should we do in this case?
	if (min_point[0] == max_point[0]) max_point[0] += 0.001;
	if (min_point[1] == max_point[1]) max_point[1] += 0.001;
	
	if(tl[0]>br[0])
	{
		tl[0]=max_point[0];
		br[0]=min_point[0];
	}
	else
	{
		br[0]=max_point[0];
		tl[0]=min_point[0];
	}
	if(tl[1]>br[1])
	{
		tl[1]=max_point[1];
		br[1]=min_point[1];
	}
	else
	{
		br[1]=max_point[1];
		tl[1]=min_point[1];
	}
	
	const int tmp_d(max(renddesc.get_w(),renddesc.get_h()));
	Real src_pw=(tmp_d*zoom_factor)/(br[0]-tl[0]);
	Real src_ph=(tmp_d*zoom_factor)/(br[1]-tl[1]);
	
	
	RendDesc desc(renddesc);
	desc.clear_flags();
	//desc.set_flags(RendDesc::PX_ASPECT);
	desc.set_tl(tl);
	desc.set_br(br);
	desc.set_wh(ceil_to_int(src_pw*(br[0]-tl[0])),ceil_to_int(src_ph*(br[1]-tl[1])));
	
	//warning("surface to render: [%f,%f]-[%f,%f] %dx%d",desc.get_tl()[0],desc.get_tl()[1],desc.get_br()[0],desc.get_br()[1],desc.get_w(),desc.get_h());
	if(desc.get_w()==0 && desc.get_h()==0)
	{
		cairo_save(cr);
		cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
		cairo_paint(cr);
		cairo_restore(cr);
		return true;
	}
	
	// Recalculate the pixel widths for the src renddesc
	src_pw=(desc.get_w())/(desc.get_br()[0]-desc.get_tl()[0]);
	src_ph=(desc.get_h())/(desc.get_br()[1]-desc.get_tl()[1]);
	
	cairo_surface_t* source=cairo_surface_create_similar(cairo_get_target(cr), CAIRO_CONTENT_COLOR_ALPHA, desc.get_w(),desc.get_h());
	cairo_surface_t* surface=cairo_surface_create_similar(cairo_get_target(cr), CAIRO_CONTENT_COLOR_ALPHA,renddesc.get_w(), renddesc.get_h());
	cairo_t* subcr=cairo_create(source);
	cairo_scale(subcr, 1/desc.get_pw(), 1/desc.get_ph());
	cairo_translate(subcr, -desc.get_tl()[0], -desc.get_tl()[1]);

	if(!context.accelerated_cairorender(subcr,quality,desc,&stageone))
		return false;
	
	cairo_destroy(subcr);
		
	int surfacew, surfaceh, sourcew, sourceh;
	
	CairoSurface csurface(surface);
	CairoSurface csource(source);
	
	csurface.map_cairo_image();
	csource.map_cairo_image();
	
	surfacew=csurface.get_w();
	surfaceh=csurface.get_h();
	sourcew=csource.get_w();
	sourceh=csource.get_h();
	
	CairoSurface::pen pen(csurface.begin());
	
	// Do the warp
	{
		int x,y;
		float u,v;
		Point point,tmp;
		for(y=0,point[1]=renddesc.get_tl()[1];y<surfaceh;y++,pen.inc_y(),pen.dec_x(x),point[1]+=1.0/ph)
		{
			for(x=0,point[0]=renddesc.get_tl()[0];x<surfacew;x++,pen.inc_x(),point[0]+=1.0/pw)
			{
				tmp=transform_forward(point);
				const float z(transform_backward_z(tmp));
				if(!clip_rect.is_inside(tmp) || !(z>0 && z<horizon))
				{
					csurface[y][x]=Color::alpha();
					continue;
				}
				
				u=(tmp[0]-tl[0])*src_pw;
				v=(tmp[1]-tl[1])*src_ph;
				
				if(u<0 || v<0 || u>=sourcew || v>=sourceh || std::isnan(u) || std::isnan(v))
					csurface[y][x]=context.get_cairocolor(tmp);
				else
				{
					// CUBIC
					if(quality<=4)
						csurface[y][x]=csource.cubic_sample_cooked(u,v);
					// INTEPOLATION_LINEAR
					else if(quality<=6)
						csurface[y][x]=csource.linear_sample_cooked(u,v);
					else
						// NEAREST_NEIGHBOR
						csurface[y][x]=csource[floor_to_int(v)][floor_to_int(u)];
				}
			}
			if((y&31)==0 && cb)
			{
				if(!stagetwo.amount_complete(y,surfaceh))
					return false;
			}
		}
	}
	
#endif
	
	if(cb && !cb->amount_complete(10000,10000)) return false;
	
	csurface.unmap_cairo_image();
	csource.unmap_cairo_image();
	cairo_surface_destroy(source);
	
	cairo_save(cr);
	
	cairo_translate(cr, renddesc.get_tl()[0], renddesc.get_tl()[1]);
	cairo_scale(cr, renddesc.get_pw(), renddesc.get_ph());
	cairo_set_source_surface(cr, surface, 0, 0);
	cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
	cairo_paint(cr);
	
	cairo_restore(cr);
	
	cairo_surface_destroy(surface);
	return true;
}

Rect
Warp::get_bounding_rect()const
{
	return Rect::full_plane();
}

Rect
Warp::get_full_bounding_rect(Context context)const
{
//	return Rect::full_plane();

	Point src_tl=param_src_tl.get(Point());
	Point src_br=param_src_br.get(Point());
	bool clip=param_clip.get(bool());

	Rect under(context.get_full_bounding_rect());

	if(clip)
	{
		under&=Rect(src_tl,src_br);
	}

	return get_transform()->perform(under);

	/*
	Rect under(context.get_full_bounding_rect());
	Rect ret(Rect::zero());

	if(under.area()==HUGE_VAL)
		return Rect::full_plane();

	ret.expand(
		transform_backward(
			under.get_min()
		)
	);
	ret.expand(
		transform_backward(
			under.get_max()
		)
	);
	ret.expand(
		transform_backward(
			Vector(
				under.get_min()[0],
				under.get_max()[1]
			)
		)
	);
	ret.expand(
		transform_backward(
			Vector(
				under.get_max()[0],
				under.get_min()[1]
			)
		)
	);

	if(ret.area()==HUGE_VAL)
		return Rect::full_plane();

	return ret;
	*/
}
