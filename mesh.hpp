#include <math.h>
#include <omp.h>
#include <vtkCleanPolyData.h>
#include <vtkContourGrid.h>
#include <vtkDoubleArray.h>
#include <vtkPolyDataConnectivityFilter.h>
#include <vtkXMLUnstructuredGridReader.h>
#include <vtkIdList.h>
#include <vtkKdTree.h>
#include <vtkExtractCells.h>
#include <vtkCellCenters.h>
#include <vtkDataArraySelection.h>
#include <vtkUnstructuredGrid.h>
#include <vtkPointData.h>

#include <chrono>
#include <iostream>
#include <string>
#include <vector>
#include <set>
#include <cstring>

#ifndef MESH_HPP
#define MESH_HPP

bool startsWith(std::string mainStr, std::string toMatch)
{
    if(mainStr.find(toMatch) == 0)
        return true;
    else
        return false;
}

vtkSmartPointer<vtkPolyData> find_best_slice(double position[3],
                                                   vtkSmartPointer<vtkPolyData> slice) {
  auto conn_filter = vtkSmartPointer<vtkPolyDataConnectivityFilter>::New();
  double min_d = 1e6;
  int rid = 0;
  double center[3] = {0.0, 0.0, 0.0};
  vtkSmartPointer<vtkPolyData> min_comp;
  conn_filter->SetInputData(slice);
  conn_filter->SetExtractionModeToSpecifiedRegions();
  while (true) {
    auto component = vtkSmartPointer<vtkPolyData>::New();
    conn_filter->AddSpecifiedRegion(rid);
    conn_filter->Update();
    component->DeepCopy(conn_filter->GetOutput());
    if (component->GetNumberOfCells() <= 0) {
      break;
    }
    conn_filter->DeleteSpecifiedRegion(rid);
    auto clean_filter = vtkSmartPointer<vtkCleanPolyData>::New();
    clean_filter->SetInputData(component);
    clean_filter->Update();
    component->DeepCopy(clean_filter->GetOutput());
    auto comp_points = component->GetPoints();
    int num_comp_points = component->GetNumberOfPoints();
    int num_comp_cells = component->GetNumberOfCells();

    double cx = 0.0;
    double cy = 0.0;
    double cz = 0.0;
    for (int i = 0; i < num_comp_points; i++) {
      double pt[3];
      comp_points->GetPoint(i, pt);
      cx += pt[0];
      cy += pt[1];
      cz += pt[2];
    }

    center[0] = cx / num_comp_points;
    center[1] = cy / num_comp_points;
    center[2] = cz / num_comp_points;
    double d = (center[0] - position[0]) * (center[0] - position[0]) +
               (center[1] - position[1]) * (center[1] - position[1]) +
               (center[2] - position[2]) * (center[2] - position[2]);

    if (d < min_d) {
      min_d = d;
      min_comp = component;
    }
    rid += 1;
  }
  return min_comp;
}


double integrate_on_slice(vtkSmartPointer<vtkPolyData> slice, int numcells,
                                std::vector<double>& area_cells,
                                std::function<double(vtkIdType)> fun) {
  double sum = 0;
  for (int j = 0; j < numcells; j++) {
    vtkIdType cell_size;
    const vtkIdType* cell_points;
    slice->GetPolys()->GetCellAtId(j, cell_size, cell_points);
    double cur_sum = 0;
    for (int k = 0; k < cell_size; k++) cur_sum += fun(cell_points[k]);
    sum += area_cells[j] * (cur_sum) / 3.;
  }
  return sum;
}

double compute_area_slice(vtkSmartPointer<vtkPolyData> slice,
                                std::vector<double>& area_cells) {
  auto polys = slice->GetPolys();
  double area = 0;
  int numcells = polys->GetNumberOfCells();
  area_cells.resize(numcells);
  for (int icell = 0; icell < numcells; icell++) {
    vtkIdType cell_size;
    const vtkIdType* cell_points;
    polys->GetCellAtId(icell, cell_size, cell_points);
    double edge_sizes[cell_size];

    // from now on, we assume that the elements are triangles (true for
    // tetrahedral mesh)

    double nodes[cell_size][3];
    for (int j = 0; j < cell_size; j++) {
      double* points = slice->GetPoints()->GetPoint(cell_points[j]);
      for (int k = 0; k < 3; k++) nodes[j][k] = points[k];
    }
    for (int j = 0; j < cell_size; j++) {
      double diff[3];
      for (int k = 0; k < 3; k++) diff[k] = nodes[(j + 1) % 3][k] - nodes[j][k];

      double sum = 0;
      for (int k = 0; k < 3; k++) sum += diff[k] * diff[k];
      edge_sizes[j] = std::sqrt(sum);
    }

    double s = (edge_sizes[0] + edge_sizes[1] + edge_sizes[2]) / 2;
    area_cells[icell] = std::sqrt(s * (s - edge_sizes[0]) *
                                  (s - edge_sizes[1]) * (s - edge_sizes[2]));
    area += area_cells[icell];
  }
  return area;
}

class Mesh {
 public:
  Mesh();
  ~Mesh();

  void read_mesh(const std::string& fileName);
  void map_on_centerline(vtkSmartPointer<vtkPolyData> centerlines,
                          bool compute_average_fields = false,
                          bool update_graphics = true);

  vtkSmartPointer<vtkUnstructuredGrid> unstructured_mesh_;
  vtkIdType num_point_arrays;

};

Mesh::Mesh() {}

Mesh::~Mesh() {}


void Mesh::read_mesh(const std::string& file_name) {
  unstructured_mesh_ = vtkSmartPointer<vtkUnstructuredGrid>::New();
  auto reader = vtkSmartPointer<vtkXMLUnstructuredGridReader>::New();
  reader->SetFileName(file_name.c_str());
  std::set<std::string> retain_data_names = {"pressure", "velocity"};
  std::vector<std::string> ign = {"vinplane_traction_", "vWSS_", "timeDeriv_",
                                  "average_speed_", "average_pressure_"};
  for (size_t i = 0; i < ign.size(); i++) {
    for (int j = 0; j < 1e4; j++) {
      std::string to_ignore = ign[i] + std::to_string(j * 5);
      reader->GetPointDataArraySelection()->DisableArray(to_ignore.c_str());
    }
  }
  reader->GetPointDataArraySelection()->DisableArray("GlobalNodeID");
  reader->GetCellDataArraySelection()->DisableArray("GlobalElementID");
  reader->Update();

  unstructured_mesh_ = reader->GetOutput();

  // Remove data arrays we don't want to generate slice data for.
  num_point_arrays =
      unstructured_mesh_->GetPointData()->GetNumberOfArrays();
  std::vector<std::string> remove_data_names;
  for (int i = 0; i < num_point_arrays; i++) {
    auto name =
        std::string(unstructured_mesh_->GetPointData()->GetArrayName(i));
    if ((startsWith(name, "velocity") == false) && (startsWith(name, "pressure") == false)) {
      remove_data_names.push_back(name);
    }
  }
  for (auto const& name : remove_data_names) {
    unstructured_mesh_->GetPointData()->RemoveArray(name.c_str());
  }
  num_point_arrays =
      unstructured_mesh_->GetPointData()->GetNumberOfArrays();

  // Add a point data array to store plane distance.
  int num_pts = unstructured_mesh_->GetNumberOfPoints();
  auto plane_dist = vtkSmartPointer<vtkDoubleArray>::New();
  plane_dist->SetName("plane_dist");
  plane_dist->SetNumberOfComponents(1);
  plane_dist->SetNumberOfTuples(num_pts);
  for (int i = 0; i < num_pts; i++) {
    plane_dist->SetValue(i, 0.0);
  }
  unstructured_mesh_->GetPointData()->AddArray(plane_dist);
}


void Mesh::map_on_centerline(vtkSmartPointer<vtkPolyData> centerline,
                              bool compute_average_fields,
                              bool update_graphics) {
  std::cout << "Prepare slice extraction" << std::endl;

  // Set the scalar field used to extract a slice.
  unstructured_mesh_->GetPointData()->SetActiveScalars("plane_dist");

  // Get centerline data.
  auto cl_points = centerline->GetPoints();
  int num_cl_points = centerline->GetNumberOfPoints();
  auto cl_normals = vtkDoubleArray::SafeDownCast(
      centerline->GetPointData()->GetArray("CenterlineSectionNormal"));
  auto cl_max_size = vtkDoubleArray::SafeDownCast(
      centerline->GetPointData()->GetArray("CenterlineSectionMaxSize"));

  // Prepare area array
  auto area = vtkSmartPointer<vtkDoubleArray>::New();
  area->SetName("area");
  area->SetNumberOfComponents(1);
  area->SetNumberOfTuples(num_cl_points);
  centerline->GetPointData()->AddArray(area);

  // Prepare pressure and velocity arrays
  for (int ipoint = 0; ipoint < num_point_arrays; ipoint++) {
    auto name = std::string(unstructured_mesh_->GetPointData()->GetArrayName(ipoint));
    if (startsWith(name, "velocity") || startsWith(name, "pressure")) {
      auto array = vtkSmartPointer<vtkDoubleArray>::New();
      array->SetName(name.c_str());
      array->SetNumberOfComponents(1);
      array->SetNumberOfTuples(num_cl_points);
      centerline->GetPointData()->AddArray(array);
    }
  }

  // Extract centers of cells and build kdtree to search for cells in radius
  auto cell_center_filter = vtkSmartPointer<vtkCellCenters>::New();
  cell_center_filter->SetInputData(unstructured_mesh_);
  cell_center_filter->Update();
  auto cell_center_kdtree = vtkSmartPointer<vtkKdTree>::New();
  cell_center_kdtree->BuildLocatorFromPoints(cell_center_filter->GetOutput()->GetPoints());

  // Start parallel extraction
  std::cout << "Extract slices" << std::endl;
  int num_slices_processed = 0;
  auto start = std::chrono::high_resolution_clock::now();
  #pragma omp parallel for schedule(dynamic)
  for (int i = 0; i < num_cl_points; i++) {
  
    // Extract thread information
    int my_slice;
    #pragma omp critical (status)
    {
      num_slices_processed++;
      my_slice = num_slices_processed;
    }

    // Extract current position
    double position1[3];
    double normal1[3];
    double maxsize1[1];
    cl_points->GetPoint(i, position1);
    cl_normals->GetTuple(i, normal1);
    cl_max_size->GetTuple(i, maxsize1);

    // Extract auxilliary second position for interpolation
    double normal2[3];
    double maxsize2[0];
    double position2[3];
    double position2a[3];
    double position2b[3];
    cl_points->GetPoint(i + 1, position2a);
    cl_points->GetPoint(i - 1, position2b);

    double quaddista = 0.0;
    double quaddistb = 0.0;
    for (int j = 0; j < 3; j++) {
      quaddista += (position1[j] - position2a[j]) * (position1[j] - position2a[j]);
      quaddistb += (position1[j] - position2b[j]) * (position1[j] - position2b[j]);
    }

    if (quaddista < quaddistb)
    {
      position2[0] = position2a[0];
      position2[1] = position2a[1];
      position2[2] = position2a[2];
      cl_normals->GetTuple(i + 1, normal2);
      cl_max_size->GetTuple(i + 1, maxsize2);
    } else {
      position2[0] = position2b[0];
      position2[1] = position2b[1];
      position2[2] = position2b[2];
      cl_normals->GetTuple(i - 1, normal2);
      cl_max_size->GetTuple(i - 1, maxsize2);
    }

    // Setup position and slice
    vtkSmartPointer<vtkPolyData> slice;
    vtkSmartPointer<vtkUnstructuredGrid> my_grid;
    double position[3];
    double normal[3];
    double srad;

    // Start slice extraction routine
    float weight = 0.01;
    while (true) {

      // Determine interpolated position, normal and max_size
      double nnorm = 0.0;
      for (int j = 0; j < 3; j++) {
        position[j] = position1[j] * (1.0 - weight) + position2[j] * weight;
        normal[j] = normal1[j] * (1.0 - weight) + normal2[j] * weight;
        nnorm += normal[j] * normal[j];
      }
      for (int j = 0; j < 3; j++) {
        normal[j] = normal[j] / nnorm;
      }
      srad = (maxsize1[0] * (1.0 - weight) + maxsize2[0] * weight) * 0.55;

      // Find cells of interest by seearching in radius around position
      auto selected_cells = vtkSmartPointer<vtkIdList>::New();
      cell_center_kdtree->FindPointsWithinRadius(srad, position, selected_cells);
      auto cell_extractor = vtkSmartPointer<vtkExtractCells>::New();
      cell_extractor->SetInputData(unstructured_mesh_);
      cell_extractor->SetCellList(selected_cells);
      cell_extractor->Update();
      my_grid = cell_extractor->GetOutput();

      // Compute distance of each mesh point from the slicing plane.
      for (int iter1 = 0; iter1 < my_grid->GetNumberOfPoints(); iter1++) {
        double pt[3];
        my_grid->GetPoints()->GetPoint(iter1, pt);
        double d[1];
        d[0] = normal[0] * (pt[0] - position[0]) +
                    normal[1] * (pt[1] - position[1]) +
                    normal[2] * (pt[2] - position[2]);
        my_grid->GetPointData()->GetArray("plane_dist")->SetTuple(iter1, d);
      }

      // Set the scalar field used to extract a slice.
      my_grid->GetPointData()->SetActiveScalars("plane_dist");

      // Extract slice with contour filter where distance to plane is zero
      auto contour = vtkSmartPointer<vtkContourGrid>::New();
      contour->SetInputData(my_grid);
      contour->SetValue(0, 0.0);
      contour->ComputeNormalsOff();
      contour->Update();
      slice = contour->GetOutput();

      // Break if extracted slice is not emtpy
      if (slice->GetNumberOfPoints()) {
        break;
      }

      // Raise runtime error if no slice found after 5 iterations
      if (weight == 0.6) {
        throw std::runtime_error("Interpolation failed.");
      }
    }

    // Find best slice
    slice = find_best_slice(position, slice.GetPointer());
    if (slice == nullptr) {
      throw std::runtime_error("Find best slice failed.");
    }

    // Extract slice data
    vtkIdType num_point_arrays_slice = slice->GetPointData()->GetNumberOfArrays();
    int num_points_slice = slice->GetNumberOfPoints();
    auto polys = slice->GetPolys();
    int numcells = polys->GetNumberOfCells();
    auto points_slice = slice->GetPoints();

    // Calculate ara of slice
    std::vector<double> area_cells;
    double area_i[1];
    area_i[0] = compute_area_slice(slice, area_cells);
    centerline->GetPointData()->GetArray("area")->SetTuple(i, area_i);
    double area_inv = 1.0 / area_i[0];

    // Iterate over point data arrays and integrate over slice
    for (int ipoint = 0; ipoint < num_point_arrays_slice; ipoint++) {

      auto name = std::string(slice->GetPointData()->GetArrayName(ipoint));

      // Integrate velocity profile
      if (startsWith(name, "velocity")) {
        double flux[num_points_slice];
        for (int j = 0; j < num_points_slice; j++) {
          double* vel =
              slice->GetPointData()->GetArray(ipoint)->GetTuple3(j);
          double curflux = 0;
          for (int k = 0; k < 3; k++) {
            curflux += vel[k] * normal[k];
          }
          flux[j] = curflux;
        }
        double result[1];
        result[0] = integrate_on_slice(
            slice, numcells, area_cells,
            [&](vtkIdType index) -> double { return flux[index]; });
        centerline->GetPointData()->GetArray(name.c_str())->SetTuple(i, result);
      }

      // Integrate pressure
      if (startsWith(name, "pressure")) {
        double result[1];
        result[0] = integrate_on_slice(
            slice, numcells, area_cells, [&](vtkIdType index) -> double {
              return slice->GetPointData()->GetArray(ipoint)->GetTuple1(
                  index);
            });
        result[0] *= area_inv;
        centerline->GetPointData()->GetArray(name.c_str())->SetTuple(i, result);
      }
    }
    std::cout << "Completed slice " << my_slice << "/" << num_cl_points << std::endl;
  }

  auto stop = std::chrono::high_resolution_clock::now();
  std::chrono::duration<double> duration = stop - start;
  std::cout << "Slice extraction completed" << std::endl;
  std::cout << "Time per slice: " << duration.count() / num_slices_processed << " s" << std::endl;
}


#endif