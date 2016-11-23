#include "Gmsh.h"
#include "GModel.h"
#include "MVertex.h"
#include "MElement.h"
#include "addalgo.hpp"
#include "trigrid.h"
#include "nan_handler.h"
#include "procgrid.h"

void TriGrid::FillFromGModel(void* gmod){
	GModel* m = static_cast<GModel*>(gmod);
	
	//--- build mesh
	//!! gmsh 2.11 doesn't work correctly without this line
	//   if chararcteristic mesh size is not 1.0
	auto bb = m->bounds();
	GmshSetBoundingBox(bb.min()[0], bb.max()[0], bb.min()[1], bb.max()[1], 0, 0);
	//GmshWriteFile("FF.opt");
	//m->writeGEO("gmsh_geo.geo");
	//shot down nan checks because gmsh has 1/0 operations in postprocessing
	NanSignalHandler::StopCheck();
	m->mesh(2);
	NanSignalHandler::StartCheck();
	//m->writeMSH("gmsh_msh.msh");
	//m->writeVTK("gmsh_msh.vtk");
	
	//--- extract mesh from gmsh
	std::map<MVertex*, GridPoint*> vrt;
	for (auto fit = m->firstFace(); fit!=m->lastFace(); ++fit){ 
		GFace* f = *fit;
		for (int i=0; i<f->getNumMeshElements(); ++i){
			auto e = f->getMeshElement(i);
			std::vector<MVertex*> vs;
			e->getVertices(vs);
			auto newcell = aa::add_shared(cells, Cell());
			for (auto v: vs){ 
				auto fnd = vrt.find(v); 
				if (fnd==vrt.end()){
					auto newp = aa::add_shared(points, GridPoint(v->x(), v->y()));
					auto ins = vrt.emplace(v, newp);
					fnd = ins.first;
				}
				add_point_to_cell(newcell, fnd->second);
			}
		}
	}

	//indicies
	set_indicies();
	//force positive triangles (necessary)
	force_cells_ordering();
}

TriGrid::TriGrid(const ContoursCollection& cont, const vector<double>& lc, double density){
	//build mesh in gmsh
	//2 - auto, 5 - delaunay, 6 - frontal
	GmshSetOption("Mesh", "Algorithm", 2.0);

	//very rough density parameter implementation: doesn't work in gmsh 2.9.1
	//double maxlc = 0;
	//for (auto v: lc) if (v>maxlc) maxlc=v;
	//GmshSetOption("Mesh", "CharacteristicLengthMax", (1.0-0.49*density)*maxlc); 

	GModel m;
	m.setFactory("Gmsh");
	//add points
	auto lcit = lc.begin();
	std::map<const Point*, GVertex*> verticies;
	for (auto i=0; i<cont.n_cont(); ++i){
		auto c = cont.get_contour(i);
		for (int j=0; j<c->n_points(); ++j){
			auto p = c->get_point(j);
			verticies[p] = m.addVertex(p->x, p->y, 0, *lcit++);
		}
	}
	//add edges and faces: each gmsh face should be single connected
	auto add_contour_edges = [&m, &verticies](std::vector<GEdge*>& e, const PContour* c){
		for (int j=0; j<c->n_points(); ++j){
			auto p = c->get_point(j);
			auto pn= c->get_point(j+1);
			e.push_back(m.addLine(verticies[p], verticies[pn]));
		}
	};
	std::vector<GFace*> fc;
	for (int i=0; i<cont.n_cont(); ++i) if (cont.is_inner(i)){
		std::vector<GEdge*> eds;
		auto c = cont.get_contour(i);
		add_contour_edges(eds, c);
		for (auto& oc: cont.get_childs(i)){
			add_contour_edges(eds, oc);
		}
		fc.push_back(m.addPlanarFace({eds}));
	}

	FillFromGModel(&m);
}

vector<HMCont2D::ExtendedTree>
TriGrid::ConstraintsPreproc(const HMCont2D::ContourTree& cont, 
		const ShpVector<HMCont2D::Contour>& constraints){
	vector<HMCont2D::ExtendedTree> ret;
	//1) sort out all inner contours
	for (auto rc: cont.nodes){
		if (HMCont2D::Contour::Area(*rc) > 0){
			ret.push_back(HMCont2D::ExtendedTree());
			auto& et = ret.back();
			shared_ptr<HMCont2D::Contour> rc2(new HMCont2D::Contour(*rc));
			et.AddContour(rc2);
			for (auto cc: rc->children){
				shared_ptr<HMCont2D::Contour> cc2(new HMCont2D::Contour(*cc));
				et.AddContour(cc2);
			}
		}
	}
	//2) check where constraint lies and add it to
	//   one of extended trees
	for (auto& c: constraints){
		for (auto p: c->ordered_points()){
			bool found = false;
			for (auto& et: ret){
				if (!et.IsWithout(*p)){
					shared_ptr<HMCont2D::Contour> sc(c);
					et.AddOpenContour(sc);
					found = true;
					break;
				}
			}
			if (found) break;
		}
	}

	/*

	//3) if point of open contour lies on bounding contours
	//   -> add this point to bounding contour
	auto split_edge = [](HMCont2D::Contour* cont, int ind, Point* point){
		Point* p1 = cont->edge(ind)->pstart;
		Point* p2 = cont->edge(ind)->pend;
		//equal nodes should have equal address
		if (*p1 == *point || *p2 == *point) {
			Point* p = (*p1 == *point) ? p1 : p2;
			auto f = cont->pinfo(p);
			if (f.eprev && f.eprev->pstart == p) f.eprev->pstart = point;
			if (f.eprev && f.eprev->pend == p) f.eprev->pend = point;
			if (f.enext && f.enext->pstart == p) f.enext->pstart = point;
			if (f.enext && f.enext->pend == p) f.enext->pend = point;
			return;
		}
		bool dircorrect = cont->correctly_directed_edge(ind);
		cont->RemoveAt({ind});
		auto e1 = std::make_shared<HMCont2D::Edge>(p1, point);
		auto e2 = std::make_shared<HMCont2D::Edge>(point, p2);
		if (dircorrect) cont->AddAt(ind, {e1, e2});
		else cont->AddAt(ind, {e2, e1});
	};
	for (auto& et: ret){
		for (auto& oc: et.open_contours){
			for (auto p: oc->all_points()){
				for (auto& bc: et.nodes){
					auto ca = bc->coord_at(*p);
					if (ISZERO(std::get<4>(ca))){
						split_edge(bc.get(), std::get<2>(ca), p);
						break;
					}
				}
			}
		}
	}
	*/

	return ret;
}

TriGrid::TriGrid(const HMCont2D::ContourTree& cont, 
		const ShpVector<HMCont2D::Contour>& constraints,
		double h){
	FillFromTree(cont, constraints, {}, std::map<Point*,double>(), h);
}

TriGrid::TriGrid(const HMCont2D::ContourTree& cont, 
		const ShpVector<HMCont2D::Contour>& constraints,
		const std::map<Point*, double>& w, double h){
	FillFromTree(cont, constraints, {}, w, h);
}

TriGrid::TriGrid(const HMCont2D::ContourTree& cont, 
		const ShpVector<HMCont2D::Contour>& constraints,
		const std::vector<double>& emb_points){
	std::map<Point*, double> w;

	vector<Point> ep; ep.reserve(emb_points.size()/3);
	for (int i=0; i<emb_points.size()/3; ++i){
		ep.push_back(Point(emb_points[3*i], emb_points[3*i+1]));
		w[&ep.back()] = emb_points[3*i+2];
	}

	FillFromTree(cont, constraints, ep, w, 0);
}

void TriGrid::CrossesProcessing(
		HMCont2D::ContourTree& cont, 
		ShpVector<HMCont2D::Contour>& constraints,
		std::map<Point*, double>& w,
		HMCont2D::PCollection& apnt,
		double h){
	auto getpw = [&](Point* p)->double{
		auto fnd = w.find(p);
		return (fnd==w.end())?h:fnd->second;
	};
	std::set<Point*> cross_points;
	auto treat_conts = [&](HMCont2D::Contour& c1, HMCont2D::Contour& c2){
		auto crosses = HMCont2D::Algos::CrossAll(c1, c2);
		for (auto& c: crosses){
			auto res1 = c1.GuaranteePoint(std::get<1>(c), apnt);
			auto res2 = c2.GuaranteePoint(std::get<1>(c), apnt);
			auto p1 = std::get<1>(res1);
			auto p2 = std::get<1>(res2);
			if (p1 == p2) continue;
			//substitute res2 with res1 pointers
			auto info1 = c1.pinfo(p1);
			auto info2 = c2.pinfo(p2);
			if (info2.eprev){
				if (info2.eprev->pstart == p2) info2.eprev->pstart = p1;
				else info2.eprev->pend = p1;
			}
			if (info2.enext){
				if (info2.enext->pstart == p2) info2.enext->pstart = p1;
				else info2.enext->pend = p1;
			}
			//set largest weight
			double cw = 0;
			if (!std::get<0>(res1)) cw = std::max(cw, getpw(p1));
			else{
				if (info1.pprev != 0) cw = std::max(cw, getpw(info1.pprev));
				if (info1.pnext != 0) cw = std::max(cw, getpw(info1.pnext));
			}
			if (!std::get<0>(res2)) cw = std::max(cw, getpw(p2));
			else{
				if (info2.pprev != 0) cw = std::max(cw, getpw(info2.pprev));
				if (info2.pnext != 0) cw = std::max(cw, getpw(info2.pnext));
			}
			w[p1] = cw;
		}
	};
	//constraints vs constraints crosses
	for (int i=0; i<constraints.size(); ++i){
		for (int j=i+1; j<constraints.size(); ++j){
			auto c1 = constraints[i];
			auto c2 = constraints[j];
			treat_conts(*c1, *c2);
		}
	}
	//constraints vs contours
	for (auto& cns: constraints){
		for (auto& tree_cont: cont.nodes){
			treat_conts(*tree_cont, *cns);
		}
	}
}

void TriGrid::FillFromTree(
		const HMCont2D::ContourTree& cont_, 
		const ShpVector<HMCont2D::Contour>& constraints_,
		const vector<Point>& emb_points,
		const std::map<Point*, double>& w_,
		double h,
		bool recomb){
	if (cont_.nodes.size() == 0) return; 

	//treat default size
	if (h<=0) h = 2*HMCont2D::ECollection::BBox(cont_).lendiag();

	//modify input data with respect to crosses with constraints
	HMCont2D::ContourTree cont = cont_;
	ShpVector<HMCont2D::Contour> constraints = constraints_;
	std::map<Point*, double> w = w_;
	HMCont2D::PCollection apnt;
	if (constraints.size() > 0){
		for (auto& c: cont.nodes) c->Reallocate();
		cont.ReloadEdges();
		for (auto& c: constraints) c->Reallocate();
		CrossesProcessing(cont, constraints, w, apnt, h);
	}

	//part tree by doubly connected ones and link each constraint with it.
	//returns ExtendedTree
	auto ap = ConstraintsPreproc(cont, constraints);

	//build mesh in gmsh
	//2 - auto, 5 - delaunay, 6 - frontal, 8 - delaunay for quads
	GmshSetOption("Mesh", "Algorithm", 2.0);
	GModel m;
	m.setFactory("Gmsh");

	//add points
	vector<const Point*> allpoints;
	for (auto& x: ap){
		auto _t = x.all_points();
		std::copy(_t.begin(), _t.end(), std::back_inserter(allpoints));
	}

	std::map<const Point*, GVertex*> verticies;
	for (auto& p: allpoints){
		auto wfnd = w.find(const_cast<Point*>(p));
		double hh = (wfnd == w.end()) ? h : wfnd->second;
		verticies[p] = m.addVertex(p->x, p->y, 0, hh);
	}
	

	//add edges and faces: each gmsh face should be single connected
	auto add_contour_edges = [&m, &verticies](const HMCont2D::Contour& c,
			vector<GEdge*>& e){
		vector<Point*> op = c.ordered_points();
		for (int i=0; i<op.size()-1; ++i){
			const Point* p = op[i];
			const Point* pn = op[i+1];
			e.push_back(m.addLine(verticies[p], verticies[pn]));
		}
	};

	//add edges
	std::vector<GFace*> fc;
	for (auto& ec: ap){
		std::vector<GEdge*> eds;
		//inner contour
		add_contour_edges(*ec.roots()[0], eds);
		//outer contours
		for (auto& child: ec.roots()[0]->children){
			add_contour_edges(*child, eds);
		}
		//assemble face
		fc.push_back(m.addPlanarFace({eds}));
		//constraints
		eds.clear();
		for (int i=ec.nodes.size(); i<ec.cont_count(); ++i){
			add_contour_edges(*ec.get_contour(i), eds);
		}
		for (auto e: eds) fc.back()->addEmbeddedEdge(e);
	}

	//add embedded points
	for (auto& p: emb_points){
		auto wfnd = w.find(const_cast<Point*>(&p));
		double hh = (wfnd == w.end()) ? h : wfnd->second;
		auto added = m.addVertex(p.x, p.y, 0, hh);
		//find face containing point
		for (int i=0; i<ap.size(); ++i){
			if (ap[i].IsWithin(p)) fc[i]->addEmbeddedVertex(added);
			break;
		}
	}

	if (recomb){
		//build 1d mesh explicitly without recombination because
		//otherwise gmsh make boundaries twice as fine
		m.mesh(1);
		//usage of delaunay for quads gives worse results for non-regular areas
		//hence using auto algorithm
		GmshSetOption("Mesh", "Algorithm", 2.0);
		GmshSetOption("Mesh", "RecombinationAlgorithm", 1.0);
		for (auto& f: fc) f->meshAttributes.recombine = 1.0;
	}

	FillFromGModel(&m);

	if (recomb){
		// if all nodes of quad grid are still trianlge than most likely builder
		// has failed. So we use another algorithm
		bool has4=false;
		for (auto c: cells) if (c->dim() == 4) {has4=true; break; }
		if (!has4){
			clear();
			GmshSetOption("Mesh", "Algorithm", 2.0);
			GmshSetOption("Mesh", "RecombinationAlgorithm", 0.0);
			for (auto& f: fc) f->meshAttributes.recombine = 1.0;
			FillFromGModel(&m);
		}
	
		//now we should check constraints lay on grid edges
		//since this feature can be not satisfied after recombination.
		if (constraints.size() > 0){
			ShpVector<HMCont2D::Edge> alledges;
			for (auto& ev: constraints) alledges.insert(alledges.end(), ev->begin(), ev->end());
			vector<Point> midpoints; midpoints.reserve(alledges.size());
			for (auto& ev: alledges) midpoints.push_back(ev->center());
			auto cf = GGeom::Info::CellFinder(this, 30, 30);
			std::map<const Cell*, int> divide_cells;
			for (size_t i=0; i<midpoints.size(); ++i){
				auto candcells = cf.CellCandidates(midpoints[i]);
				Point* pstart = alledges[i]->pstart;
				Point* pend = alledges[i]->pend;
				for (auto cand: candcells) if (cand->dim() == 4){
					auto cc = GGeom::Info::CellContour(*this, cand->get_ind());
					if (cc.IsWithin(midpoints[i])){
						int ep1=-1, ep2=-1;
						for (int j=0; j<cand->dim(); ++j){
							auto p1 = cand->get_point(j);
							if (*p1 == *pstart) ep1 = j;
							if (*p1 == *pend) ep2 = j;
						}
						if (ep1>=0 && ep2>=0){
							if (ep2 < ep1) std::swap(ep1, ep2);
							if ((ep2-ep1) % 2 == 0){
								auto fnd = divide_cells.find(cand);
								if (fnd != divide_cells.end()){
									fnd->second = -1;
								} else {
									divide_cells.emplace(cand, ep1);
								}
							}
						}
						break;
					}
				}
			}
			for (auto kv: divide_cells) if (kv.second>=0){
				GridPoint* p0 = kv.first->points[(kv.second) % 4];
				GridPoint* p1 = kv.first->points[(kv.second+1) % 4];
				GridPoint* p2 = kv.first->points[(kv.second+2) % 4];
				GridPoint* p3 = kv.first->points[(kv.second+3) % 4];
				cells[kv.first->get_ind()]->points = {p0, p1, p2};
				auto newcell = aa::add_shared(cells, Cell());
				newcell->points = {p0, p2, p3};
			}
			set_cell_indicies();
		}

	}
}

shared_ptr<TriGrid> TriGrid::FromGmshGeo(const char* fn){
	GmshSetOption("Mesh", "Algorithm", 2.0);
	GModel m;
	m.setFactory("Gmsh");
	m.load(fn);
	shared_ptr<TriGrid> ret(new TriGrid());
	ret->FillFromGModel(&m);
	return ret;
}

std::set<Edge>& TriGrid::edges() const{
	if (_edges.size()==0) _edges = get_edges();
	return _edges;
}
std::map<GridPoint*, vector<GridPoint*>>& TriGrid::nodenodeI() const{
	if (_nodenodeI.size()==0){
		//1) get nodenode
		auto& eds = edges();
		for (auto& e: eds){
			auto it1 = _nodenodeI.emplace(points[e.p1].get(), vector<GridPoint*>());
			it1.first->second.push_back(points[e.p2].get());
			auto it2 = _nodenodeI.emplace(points[e.p2].get(), vector<GridPoint*>());
			it2.first->second.push_back(points[e.p1].get());
		}
		//2) remove boundary
		for (auto& e: eds) if (e.is_boundary()){
			auto it1 = _nodenodeI.find(points[e.p1].get());
			auto it2 = _nodenodeI.find(points[e.p2].get());
			if (it1!=_nodenodeI.end()) _nodenodeI.erase(it1);
			if (it2!=_nodenodeI.end()) _nodenodeI.erase(it2);
		}
	}
	return _nodenodeI;
}

ShpVector<Point> TriGrid::ref_points(const vector<double>& dists, double density) const{
	auto ret=ShpVector<Point>();
	for (auto e: edges()){
		if (!e.is_boundary()){
			auto p1 = get_point(e.p1), p2 = get_point(e.p2);
			double len = Point::dist(*p1, *p2);
			auto ksi = RefineSection(dists[e.p1], dists[e.p2], len, density);
			for (auto k: ksi) aa::add_shared(ret, Point::Weigh(*p1, *p2, k/len)); 
		}
	}
	return ret;
}

void TriGrid::smooth(double w){
	for (auto& nn: nodenodeI()){
		Point wav(0,0);
		for (auto& an: nn.second) wav+=*an;
		wav*=(w/nn.second.size());
		(*nn.first)*=(1-w); (*nn.first)+=wav;
	}
}

vector<Point> TriGrid::cell_centers() const{
	vector<Point> ret;
	for (auto& c: cells){
		auto p = Point(0,0);
		p += *c->get_point(0);
		p += *c->get_point(1);
		p += *c->get_point(2);
		p/=3;
		ret.push_back(p);
	}
	return ret;
}

vector<double> TriGrid::cell_areas() const{
	vector<double> ret;
	for (auto& c: cells){
		ret.push_back(triarea(*c->get_point(0), *c->get_point(1), *c->get_point(2)));
	}
	return ret;
}

shared_ptr<TriGrid>
TriGrid::TriangulateArea(const vector<Point>& pts, double h){
	Contour c(pts);
	ContoursCollection cc({c});
	vector<double> lc (c.n_points(), h);
	shared_ptr<TriGrid> ret(new TriGrid(cc, lc, 7.0));
	return ret;
}

shared_ptr<TriGrid>
TriGrid::TriangulateArea(const vector<vector<Point>>& pts, double h){
	vector<PContour> c;
	for (auto& x: pts){
		PContour pc;
		for (auto& x2: x) pc.add_point(const_cast<Point*>(&x2));
		c.push_back(pc);
	}
	ContoursCollection cc(c);
	int np = 0;
	for (auto& x: pts) np += x.size();
	vector<double> lc (np, h);
	shared_ptr<TriGrid> ret(new TriGrid(cc, lc, 7.0));
	return ret;
}

shared_ptr<TriGrid>
TriGrid::TriangulateAreaConstrained(const vector<vector<Point>>& bnd,
		const vector<vector<Point>>& cns, double h){
	ShpVector<HMCont2D::Container<HMCont2D::Contour>> contours;
	ShpVector<HMCont2D::Container<HMCont2D::Contour>> constraints;
	for (auto& p: bnd) aa::add_shared(
			contours,
			HMCont2D::Constructor::ContourFromPoints(p, true)
	);

	for (auto& p: cns) aa::add_shared(
			constraints,
			HMCont2D::Constructor::ContourFromPoints(p, false)
	);

	ShpVector<HMCont2D::Contour> ccontours;
	for (auto v: contours) ccontours.push_back(v);
	ShpVector<HMCont2D::Contour> cconstraints;
	for (auto v: constraints) cconstraints.push_back(v);


	HMCont2D::ContourTree tree;
	for (auto& p: ccontours){
		tree.AddContour(p);
	}


	//build
	shared_ptr<TriGrid> ret(new TriGrid(tree, cconstraints, h));
	return ret;
}

shared_ptr<TriGrid>
TriGrid::TriangulateArea(const HMCont2D::ContourTree& cont, double h){
	return shared_ptr<TriGrid>(new TriGrid(cont, {}, h));
}

shared_ptr<TriGrid>
TriGrid::TriangulateArea(const HMCont2D::ContourTree& cont, const std::map<Point*, double>& w, double h){
	return shared_ptr<TriGrid>(new TriGrid(cont, {}, w, h));
}


GridGeom QuadGrid(const HMCont2D::ContourTree& cont, 
		const ShpVector<HMCont2D::Contour>& constraints,
		const std::vector<double>& emb_points){
	std::map<Point*, double> w;

	vector<Point> ep; ep.reserve(emb_points.size()/3);
	for (int i=0; i<emb_points.size()/3; ++i){
		ep.push_back(Point(emb_points[3*i], emb_points[3*i+1]));
		w[&ep.back()] = emb_points[3*i+2];
	}

	TriGrid g;
	g.FillFromTree(cont, constraints, ep, w, 0, true);
	return GridGeom(std::move(g));
}

shared_ptr<GridGeom> QuadrangulateArea(const HMCont2D::ContourTree& cont,
		const std::map<Point*, double>& w, double h){
	TriGrid g;
	g.FillFromTree(cont, {}, {}, w, 2, true);
	shared_ptr<GridGeom> ret(new GridGeom(std::move(g)));
	return ret;
}

