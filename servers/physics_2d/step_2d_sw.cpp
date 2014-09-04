/*************************************************************************/
/*  step_2d_sw.cpp                                                       */
/*************************************************************************/
/*                       This file is part of:                           */
/*                           GODOT ENGINE                                */
/*                    http://www.godotengine.org                         */
/*************************************************************************/
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                 */
/*                                                                       */
/* Permission is hereby granted, free of charge, to any person obtaining */
/* a copy of this software and associated documentation files (the       */
/* "Software"), to deal in the Software without restriction, including   */
/* without limitation the rights to use, copy, modify, merge, publish,   */
/* distribute, sublicense, and/or sell copies of the Software, and to    */
/* permit persons to whom the Software is furnished to do so, subject to */
/* the following conditions:                                             */
/*                                                                       */
/* The above copyright notice and this permission notice shall be        */
/* included in all copies or substantial portions of the Software.       */
/*                                                                       */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,       */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF    */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.*/
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY  */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,  */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE     */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                */
/*************************************************************************/
#include "step_2d_sw.h"


void Step2DSW::_populate_island(Body2DSW* p_body,Body2DSW** p_island,Constraint2DSW **p_constraint_island) {

	p_body->set_island_step(_step);
	p_body->set_island_next(*p_island);
	*p_island=p_body;

	for(Map<Constraint2DSW*,int>::Element *E=p_body->get_constraint_map().front();E;E=E->next()) {		

		Constraint2DSW *c=(Constraint2DSW*)E->key();
		if (c->get_island_step()==_step)
			continue; //already processed
		c->set_island_step(_step);
		c->set_island_next(*p_constraint_island);
		*p_constraint_island=c;


		for(int i=0;i<c->get_body_count();i++) {
			if (i==E->get())
				continue;
			Body2DSW *b = c->get_body_ptr()[i];
			if (b->get_island_step()==_step || b->get_mode()==Physics2DServer::BODY_MODE_STATIC || b->get_mode()==Physics2DServer::BODY_MODE_KINEMATIC)
				continue; //no go
			_populate_island(c->get_body_ptr()[i],p_island,p_constraint_island);
		}
	}
}

void Step2DSW::_setup_island(Constraint2DSW *p_island,float p_delta) {

	Constraint2DSW *ci=p_island;
	while(ci) {
		bool process = ci->setup(p_delta);
		//todo remove from island if process fails
		ci=ci->get_island_next();
	}
}

void Step2DSW::_solve_island(Constraint2DSW *p_island,int p_iterations,float p_delta){


	for(int i=0;i<p_iterations;i++) {

		Constraint2DSW *ci=p_island;
		while(ci) {
			ci->solve(p_delta);
			ci=ci->get_island_next();
		}
	}
}

void Step2DSW::_check_suspend(Body2DSW *p_island,float p_delta) {


	bool can_sleep=true;

	Body2DSW *b = p_island;
	while(b) {

		if (b->get_mode()==Physics2DServer::BODY_MODE_STATIC || b->get_mode()==Physics2DServer::BODY_MODE_KINEMATIC) {
			b=b->get_island_next();
			continue; //ignore for static
		}

		if (!b->sleep_test(p_delta))
			can_sleep=false;

		b=b->get_island_next();
	}

	//put all to sleep or wake up everyoen

	b = p_island;
	while(b) {

		if (b->get_mode()==Physics2DServer::BODY_MODE_STATIC || b->get_mode()==Physics2DServer::BODY_MODE_KINEMATIC) {
			b=b->get_island_next();
			continue; //ignore for static
		}

		bool active = b->is_active();

		if (active==can_sleep)
			b->set_active(!can_sleep);

		b=b->get_island_next();
	}
}

void Step2DSW::step(Space2DSW* p_space,float p_delta,int p_iterations) {


	p_space->lock(); // can't access space during this

	p_space->setup(); //update inertias, etc

	const SelfList<Body2DSW>::List * body_list = &p_space->get_active_body_list();

	/* INTEGRATE FORCES */
	int active_count=0;

	const SelfList<Body2DSW>*b = body_list->first();
	while(b) {

		b->self()->integrate_forces(p_delta);
		b=b->next();
		active_count++;
	}

	p_space->set_active_objects(active_count);

	/* GENERATE CONSTRAINT ISLANDS */

	Body2DSW *island_list=NULL;
	Constraint2DSW *constraint_island_list=NULL;
	b = body_list->first();

	int island_count=0;

	while(b) {
		Body2DSW *body = b->self();


		if (body->get_island_step()!=_step) {

			Body2DSW *island=NULL;
			Constraint2DSW *constraint_island=NULL;
			_populate_island(body,&island,&constraint_island);

			island->set_island_list_next(island_list);
			island_list=island;

			if (constraint_island) {
				constraint_island->set_island_list_next(constraint_island_list);
				constraint_island_list=constraint_island;
				island_count++;
			}

		}
		b=b->next();
	}

	p_space->set_island_count(island_count);

	const SelfList<Area2DSW>::List &aml = p_space->get_moved_area_list();



	while(aml.first()) {
		for(const Set<Constraint2DSW*>::Element *E=aml.first()->self()->get_constraints().front();E;E=E->next()) {

			Constraint2DSW*c=E->get();
			if (c->get_island_step()==_step)
				continue;
			c->set_island_step(_step);
			c->set_island_next(NULL);
			c->set_island_list_next(constraint_island_list);
			constraint_island_list=c;
		}
		p_space->area_remove_from_moved_list((SelfList<Area2DSW>*)aml.first()); //faster to remove here
	}

//	print_line("island count: "+itos(island_count)+" active count: "+itos(active_count));
	/* SETUP CONSTRAINT ISLANDS */

	{
		Constraint2DSW *ci=constraint_island_list;
		while(ci) {

			_setup_island(ci,p_delta);
			ci=ci->get_island_list_next();
		}
	}

	/* SOLVE CONSTRAINT ISLANDS */

	{
		Constraint2DSW *ci=constraint_island_list;
		while(ci) {
			//iterating each island separatedly improves cache efficiency
			_solve_island(ci,p_iterations,p_delta);
			ci=ci->get_island_list_next();
		}
	}

	/* INTEGRATE VELOCITIES */

	b = body_list->first();
	while(b) {

		const SelfList<Body2DSW>*n=b->next();
		b->self()->integrate_velocities(p_delta);
		b=n;  // in case it shuts itself down
	}

	/* SLEEP / WAKE UP ISLANDS */

	{
		Body2DSW *bi=island_list;
		while(bi) {

			_check_suspend(bi,p_delta);
			bi=bi->get_island_list_next();
		}
	}

	p_space->update();
	p_space->unlock();
	_step++;



}

Step2DSW::Step2DSW() {

	_step=1;
}
