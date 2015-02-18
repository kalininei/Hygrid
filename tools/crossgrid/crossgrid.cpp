#include "crossgrid.h"
#include "grid.h"
#include "fileproc.h"
#include "wireframegrid.h"
#include "gmsh/Gmsh.h"

namespace{

//gmesh library initialization
struct _gmsh_initializer{
	_gmsh_initializer(){GmshInitialize();}
	~_gmsh_initializer(){GmshFinalize();}
};
_gmsh_initializer gi;

}

Grid* grid_construct(int Npts, int Ncells, double* pts, int* cells){
	return new GridGeom(Npts, Ncells, pts, cells);
}

int grid_npoints(Grid* g){
	return static_cast<GridGeom*>(g)->n_points();
}

int grid_ncells(Grid* g){
	return static_cast<GridGeom*>(g)->n_cells();
}
int grid_cellsdim(Grid* g){
	return static_cast<GridGeom*>(g)->n_cellsdim();
}

void grid_get_points_cells(Grid* g, double* pts, int* cells){
	auto gg=static_cast<GridGeom*>(g);
	//points
	for (int i=0; i<gg->n_points(); ++i){
		auto p = gg->get_point(i);
		*pts++ = p->x;
		*pts++ = p->y;
	}
	//cells
	for (int i=0; i<gg->n_cells(); ++i){
		auto c = gg->get_cell(i);
		*cells++ = c->dim();
		for (int j=0; j<c->dim(); ++j){
			*cells++ = c->get_point(j)->get_ind();
		}
	}
}

void grid_save_vtk(Grid* g, const char* fn){
	save_vtk(static_cast<GridGeom*>(g), fn);
}

void grid_free(Grid* g){
	delete g;
}

Grid* cross_grids(Grid* gbase, Grid* gsecondary, double buffer_size, double density){
	return GridGeom::cross_grids(
			static_cast<GridGeom*>(gbase),
			static_cast<GridGeom*>(gsecondary),
			buffer_size, density);
}

// ========================== testing
namespace{

void add_check(bool ex, const char* info = 0){
	if (info==0) std::cout<<"\tunknown check: ";
	else std::cout<<"\t"<<info;
	if (ex) std::cout<<": True"<<std::endl;
	else std::cout<<": False <<<<<<<<<<<<<<<<<<<"<<std::endl;
};

//build a rectangular structured grid 
GridGeom rectangular_grid(double x0, double y0,
		double x1, double y1, int Nx, int Ny){
	double hx = (x1 - x0)/Nx;
	double hy = (y1 - y0)/Ny;
	//points
	std::vector<double> pts;
	for (int j=0; j<Ny+1; ++j){
		for (int i=0;i<Nx+1;++i){
			pts.push_back(i*hx+x0);
			pts.push_back(j*hy+y0);
		}
	}
	//cells
	auto pts_ind = [Nx, Ny](int i, int j){
		return j*(Nx+1)+i;
	};
	std::vector<int> cls;
	for (int j=0; j<Ny; ++j){
		for (int i=0; i<Nx; ++i){
			cls.push_back(4);
			cls.push_back(pts_ind(i,j));
			cls.push_back(pts_ind(i+1,j));
			cls.push_back(pts_ind(i+1,j+1));
			cls.push_back(pts_ind(i,j+1));
		}
	}
	return GridGeom((Nx+1)*(Ny+1), Nx*Ny, &pts[0], &cls[0]);
}

void test1(){
	std::cout<<"contours collection tests"<<std::endl;
	Contour c1, c2, c3, c4, c5;
	c1.add_point(0,0); c1.add_point(1,0);
	c1.add_point(1,1); c1.add_point(0,1);
	c2.add_point(0.2, 0.3); c2.add_point(0.1, 0.1);
	c2.add_point(0.1, 0.9); c2.add_point(0.9, 0.9);
	c3.add_point(0.3, 0.8); c3.add_point(0.2, 0.8);
	c3.add_point(0.2, 0.7); c3.add_point(0.3, 0.7);
	c4.add_point(0.5, 0.1); c4.add_point(0.8, 0.1);
	c4.add_point(0.8, 0.4);

	ContoursCollection cc;
	cc.add_contour(c2); 
	cc.add_contour(c1);
	cc.add_contour(c4);
	cc.add_contour(c3);

	add_check(cc.is_inside(Point(0,0))==0, "point within");
	add_check(cc.is_inside(Point(-1,0))==-1, "point within");
	add_check(cc.is_inside(Point(0.3,0.1))==1, "point within");
	add_check(cc.is_inside(Point(0.6,0.1))==0, "point within");
	add_check(cc.is_inside(Point(0.3,0.6))==-1, "point within");
	add_check(cc.is_inside(Point(0.25,0.75))==1, "point within");

	c5.add_point(0.9, 0.05); c5.add_point(0.9, 0.6);
	c5.add_point(0.3, 0.05);
	add_check(cc.is_inside(Point(0.7, 0.2))==-1, "point within");
	cc.add_contour(c5);
	add_check(cc.is_inside(Point(0.7, 0.2))== 1, "point within");
}

void test2(){
	std::cout<<"PtsGraph cut test"<<std::endl;
	double pts[] = {0,0,9,2,8,6,4,1};
	int cls[] = {3,0,3,2, 3,3,1,2};
	GridGeom G(4, 2, pts, cls);
	PtsGraph gr(G);

	Contour c;
	c.add_point(2,-1); c.add_point(9,-1);
	c.add_point(4, 5); c.add_point(2, 5);
	ContoursCollection cc({c});
	PtsGraph newgraph = PtsGraph::cut(gr, cc, 1);
	GridGeom newgeom =  newgraph.togrid();
	//save_vtk(&gr, "graph.vtk");
	//save_vtk(&newgraph, "cutgraph.vtk");
	//save_vtk(&newgeom, "cutgrid.vtk");
	add_check(newgeom.n_cells()==4 && newgeom.n_points()==10, "grid geometry");
}

void test3(){
	std::cout<<"Grid combine test"<<std::endl;
	double pts1[] = {0,0, 2,2, 0,4, -2,2, 0,2};
	double pts2[] = {-1,1, 1,1, 1, 2.5, -1,3};
	int cls1[] = {3,0,1,4, 3,4,1,2, 3,3,4,2, 3,3,0,4};
	int cls2[] = {4,0,1,2,3};
	GridGeom gmain(5,4, pts1, cls1);
	GridGeom gsec(4,1, pts2, cls2);
	GridGeom* comb = GridGeom::combine(&gmain, &gsec);
	add_check(comb->n_cells()==8 && comb->n_points()==12, "grid geometry");
	//save_vtk(comb, "combined_grid.vtk");
	delete comb;
}

void test4(){
	std::cout<<"Grid subdivide"<<std::endl;
	auto grid1 = rectangular_grid(0, 0, 1, 1, 10, 10);
	auto grid2 = rectangular_grid(1, 1, 2, 2, 10, 10);
	auto grid3 = rectangular_grid(2,1.85, 3, 2.85, 10, 10);
	auto cross1 = GridGeom::cross_grids(&grid1, &grid2, 0.0, 0.5);
	auto cross2 = GridGeom::cross_grids(cross1, &grid3, 0.0, 0.5);
	save_vtk(cross2, "test4_grid.vtk");
	add_check(cross2->n_points()==362 && cross2->n_cells()==300, "combined grid topology");
	auto div = cross2->subdivide();
	add_check(div.size()==2, "number of single connected grids");
	add_check(div[0]->n_points()==121 && div[0]->n_cells()==100, "grid1 topology");
	add_check(div[1]->n_points()==242 && div[1]->n_cells()==200, "grid2 topology");
	delete cross1;
	delete cross2;
}


}

void crossgrid_internal_tests(){
	std::cout<<"crossgrid shared library internal tests ================"<<std::endl;
	test1();
	test2();
	test3();
	test4();
	std::cout<<"crossgrid shared library internal tests: DONE =========="<<std::endl;
}

