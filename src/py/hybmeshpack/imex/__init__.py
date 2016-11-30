import xml.etree.ElementTree as ET
import writexml
import readxml
import gridexport
import contexport
import grid3export
from hybmeshpack import gdata
from hybmeshpack.basic.interf import Callback
from hybmeshpack.gdata import contour2
from hybmeshpack.gdata import grid2
from hybmeshpack import hmcore as hmcore
from hybmeshpack.hmcore import g2 as g2core
from hybmeshpack.hmcore import c2 as c2core
from hybmeshpack.hmcore import g3 as g3core
from hybmeshpack.hmcore import s3 as s3core


def write_flow_and_framework_to_file(comflow, filename, fmt="ascii"):
    'writes flow.CommandFlow comflow to xml file'
    # write everithing except geometry data data
    r = writexml._root_xml()
    writexml.write_command_flow(comflow, r)
    st = ET.SubElement(r.find('FLOW'), "STATE")
    writexml.write_framework(comflow.get_receiver(), st)
    writexml.writexml(r, filename, False)
    # add geometry data and write to file
    c_writer = 0
    try:
        c_writer = hmcore.hmxml_read(filename)
        hmcore.hmxml_change_basenode(c_writer, "FLOW/STATE")
        export_all(filename, fmt, comflow, c_writer)
    except:
        raise
    finally:
        cb = comflow.get_interface().ask_for_callback(Callback.CB_CANCEL2)
        cb._callback("Save to file", "", 0.8, 0)
        hmcore.hmxml_finalize(c_writer, filename) if c_writer != 0 else None
        cb._callback("", "Done", 1, 1)


def read_flow_and_framework_from_file(filename, flow):
    '-> CommandFlow'
    c_reader = 0
    try:
        flow.to_zero_state()
        c_reader = hmcore.hmxml_read(filename)
        pstring = hmcore.hmxml_purged_string(c_reader)
        root = ET.fromstring(pstring)
        flow_nodes = root.findall('FLOW')
        if len(flow_nodes) == 0:
            raise Exception('No proper data in %s' % filename)
        flow_node = flow_nodes[0]
        readxml.load_command_flow(flow, flow_node)
        state_node = flow_node.find('STATE')
        data = gdata.Framework()
        if state_node is not None:
            readxml.load_framework_state(data, state_node)
            hmcore.hmxml_change_basenode(c_reader, "FLOW/STATE")
            cb = flow.get_interface().ask_for_callback(Callback.CB_CANCEL2)
            c, g, g3, s3, cn, gn, g3n, s3n = import_all(c_reader, cb)
            for k, v in zip(cn, c):
                data.add_ucontour(k, v)
            for k, v in zip(gn, g):
                data.add_grid(k, v)
            for k, v in zip(g3n, g3):
                data.add_grid3(k, v)
            for k, v in zip(s3n, s3):
                data.add_usurface(k, v)
        flow.set_receiver(data)
    except:
        raise
    finally:
        hmcore.hmxml_free_node(c_reader) if c_reader != 0 else None


def export_grid(fmt, fn, name, fw=None, flow=None, adata=None):
    """exports grid from framework fw or flow receiver to
    filename fn using format fmt. Possible formats:
        vtk, hmg, msh, ggen, gmsh, tecplot,
        vtk3d, msh3d

    adata - additional data which dependes on format:
      * msh, msh3d: define periodic conditions

        msh   - [btype_per, btype_shadow, is_reversed,
                 .....]
        msh3d - [btype_per, btype_shadow, Point btype_per, Point btype_shadow,
                 .....]
      * hmg, hmg3d: defines format and a list of additional fields to export:
           {"fmt": format,
            "afields": [list of fields],
            "writer": hmcore.writer if needed}
    """
    # 1. Find grid
    try:
        if fw is None:
            fw = flow.get_receiver()
        if not isinstance(name, list):
            name = [name]
        grid = []
        for nm in name:
            if (fmt[-2:] == '3d'):
                _, _, g = fw.get_grid3(name=nm)
            else:
                _, _, g = fw.get_grid(name=nm)
            grid.append(g)

        if fmt[:3] == 'hmg':
            gsum = grid
        elif len(grid) == 1:
            gsum = grid[0]
        else:
            if fmt[-2:] == '3d':
                raise Exception("exporting list of 3d grids is not "
                                "implemented")
            else:
                gsum = grid2.Grid2()
                for g in grid:
                    gsum.add_from_grid(g)

        if flow is not None:
            callb = flow.get_interface().ask_for_callback(Callback.CB_CANCEL2)
        else:
            callb = None
    except:
        raise
    # 2. Export regarding to format
    if fmt == 'vtk':
        gridexport.vtk(gsum, fn)
    elif fmt == 'hmg':
        hmgfmt = adata['fmt'] if 'fmt' in adata else 'ascii'
        hmgaf = adata['afields'] if 'afields' in adata else None
        wr = adata['writer'] if 'writer' in adata else None
        gridexport.hmg(gsum, name, fn, hmgfmt, hmgaf, wr)
    elif fmt == 'msh':
        gridexport.msh(gsum, fn, fw.boundary_types, adata)
    elif fmt == 'ggen':
        gridexport.ggen(gsum, fn)
    elif fmt == "gmsh":
        gridexport.gmsh(gsum, fn, fw.boundary_types)
    elif fmt == "tecplot":
        gridexport.tecplot(gsum, fn, fw.boundary_types)
    elif fmt == "vtk3d":
        grid3export.vtk(gsum, fn, callb)
    elif fmt == "msh3d":
        grid3export.msh(gsum, fn, fw.boundary_types, callb, adata)
    elif fmt == "gmsh3d":
        grid3export.gmsh(gsum, fn, fw.boundary_types, callb)
    elif fmt == "tecplot3d":
        grid3export.tecplot(gsum, fn, fw.boundary_types, callb)
    elif fmt == "hmg3d":
        hmgfmt = adata['fmt'] if 'fmt' in adata else 'ascii'
        hmgaf = adata['afields'] if 'afields' in adata else None
        wr = adata['writer'] if 'writer' in adata else None
        grid3export.hmg(gsum, name, fn, hmgfmt, hmgaf, callb, wr)
    else:
        raise Exception('Unknown grid format %s' % fmt)


def export_contour(fmt, fn, name, fw=None, flow=None, adata=None):
    """exports contour from framework 'fw' or 'flow' receiver to
    filename fn using format fmt. Possible formats:
        vtk, tecplot, hmc
    """
    # Find contour
    try:
        if fw is None:
            fw = flow.get_receiver()
        contlist = []
        names = []
        if isinstance(name, list):
            cont = contour2.Contour2()
            for nm in name:
                try:
                    _, _, c = fw.get_ucontour(name=nm)
                    names.append(nm)
                except KeyError:
                    c = fw.get_grid(name=nm)[2].cont
                    names.append("ContourOf" + nm)
                contlist.append(c)
                cont.add_from_abstract(c)
        else:
            try:
                _, _, cont = fw.get_ucontour(name=name)
                names = [name]
            except KeyError:
                cont = fw.get_grid(name=name)[2].cont
                names = ["ContourOf" + name]
            contlist.append(cont)
    except:
        raise Exception('Can not find contour for exporting')
    # 2. Export regarding to format
    if fmt == 'vtk':
        contexport.vtk(cont, fn)
    elif fmt == 'hmc':
        wr = adata['writer'] if 'writer' in adata else None
        contexport.hmc(contlist, names, fn, adata['fmt'], wr)
    elif fmt == 'tecplot':
        contexport.tecplot(cont, fn, fw.boundary_types)
    else:
        raise Exception('Unknown contour format %s' % fmt)


def export_grid3_surface(fmt, fn, name, fw=None, flow=None):
    # Find grid
    try:
        if fw is None:
            fw = flow.get_receiver()
        _, _, grid = fw.get_grid3(name=name)
        if flow is not None:
            callb = flow.get_interface().ask_for_callback(Callback.CB_CANCEL2)
        else:
            callb = None
    except:
        raise Exception('Can not find contour for exporting')
    # 2. Export regarding to format
    if fmt == 'vtk':
        grid3export.vtk_surface(grid, fn, callb)
    else:
        raise Exception('Unsupported format %s' % fmt)


def export_all(fname, fmt, flow, wr=None):
    c_writer = 0
    try:
        c_writer = hmcore.hmxml_new() if wr is None else wr
        fw = flow.get_receiver()
        allconts = fw.get_ucontour_names()
        allgrids = fw.get_grid_names()
        allgrids3 = fw.get_grid3_names()
        cb = flow.get_interface().ask_for_callback(Callback.CB_CANCEL2)
        adata = {"fmt": fmt, "afields": [], "writer": c_writer}
        # contours
        cb._callback("Write contours", "", 0, 0)
        export_contour("hmc", fname, allconts, fw, None, adata)
        # grids
        cb._callback("Write grids", "", 0.1, 0)
        export_grid("hmg", fname, allgrids, fw, None, adata)
        # grids 3d
        cb._callback("Write grids 3d", "", 0.3, 0)
        export_grid("hmg3d", fname, allgrids3, fw, None, adata)
    except:
        raise
    finally:
        if wr is None:
            cb._callback("Save to file", "", 0.8, 0)
            hmcore.hmxml_finalize(c_writer, fname) if c_writer != 0 else None
            cb._callback("", "Done", 1, 1)


def import_all(reader, cb):
    """ -> [contours], [grids], [grids3d], [surfaces3d]
            [contour names], [grids names], [grids3d names], [srf names] """
    c_greader, c_creader, c_g3reader, c_s3reader = [], [], [], []
    try:
        cb._callback("Loading xml file", "", 0, 0)
        # find node
        c_greader = hmcore.hmxml_query(reader, "GRID2D")
        c_creader = hmcore.hmxml_query(reader, "CONTOUR2D")
        c_g3reader = hmcore.hmxml_query(reader, "GRID3D")
        c_s3reader = hmcore.hmxml_query(reader, "SURFACE3D")
        # contours allocation
        conts, cnames = [None] * len(c_creader), [None] * len(c_creader)
        # grids allocation
        grids, gnames = [None] * len(c_greader), [None] * len(c_greader)
        # grids3d allocation
        grids3, g3names = [None] * len(c_g3reader), [None] * len(c_g3reader)
        # surfaces3d allocation
        srf3, s3names = [None] * len(c_s3reader), [None] * len(c_s3reader)

        cbtotal = len(c_g3reader) + 3
        # read contours
        cb.subcallback(0, cbtotal)._callback("Reading contours", "", 0, 0)
        for i in range(len(c_creader)):
            conts[i], cnames[i] =\
                c2core.contour_from_hmxml(reader, c_creader[i])
        # read grids
        cb.subcallback(1, cbtotal)._callback("Reading grids", "", 0, 0)
        for i in range(len(c_greader)):
            grids[i], gnames[i] =\
                g2core.grid_from_hmxml(reader, c_greader[i])
        # read 3d grids
        for i in range(len(c_g3reader)):
            subcb = cb.subcallback(i + 2, cbtotal)
            grids3[i], g3names[i] =\
                g3core.grid_from_hmxml(reader, c_g3reader[i], subcb)
        # read surfaces
        cb.subcallback(cbtotal - 1, cbtotal)._callback(
            "Reading surfaces", "", 0, 0)
        for i in range(len(c_s3reader)):
            srf3[i], s3names[i] =\
                s3core.surface_from_hmxml(reader, c_s3reader[i])
        return conts, grids, grids3, srf3, cnames, gnames, g3names, s3names
    except:
        raise
    finally:
        for gr in c_greader + c_creader + c_g3reader + c_s3reader:
            hmcore.hmxml_free_node(gr)
