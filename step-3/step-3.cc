#include <iostream>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <deal.II/base/function_parser.h>
#include <deal.II/base/quadrature_lib.h>

#include <deal.II/dofs/dof_handler.h>
#include <deal.II/dofs/dof_tools.h>

#include <deal.II/fe/fe_q.h>
#include <deal.II/fe/fe_values.h>

#include <deal.II/grid/grid_generator.h>
#include <deal.II/grid/manifold_lib.h>

#include <deal.II/lac/dynamic_sparsity_pattern.h>
#include <deal.II/lac/constraint_matrix.h>

#include <deal.II/numerics/data_out.h>
#include <deal.II/numerics/matrix_tools.h>

// for distributed computations
#include <deal.II/base/utilities.h>
#include <deal.II/base/index_set.h>
#include <deal.II/distributed/tria.h>
#include <deal.II/distributed/grid_refinement.h>
#include <deal.II/lac/sparsity_tools.h>

#include <deal.II/lac/trilinos_solver.h>
#include <deal.II/lac/trilinos_sparse_matrix.h>
#include <deal.II/lac/trilinos_precondition.h>
#include <deal.II/lac/trilinos_vector.h>

#include <deal.II/numerics/vector_tools.h>

#include "../common/system_matrix.h"
#include "../common/system_rhs.h"
#include "../common/parameters.h"

using namespace dealii;

constexpr int manifold_id {0};


template<int dim>
class CDRProblem
{
public:
  CDRProblem(const CDR::Parameters &parameters);
  void run();
private:
  const CDR::Parameters parameters;
  const double time_step;

  MPI_Comm mpi_communicator;
  unsigned int n_mpi_processes;
  unsigned int this_mpi_process;

  FE_Q<dim> fe;
  QGauss<dim> quad;
  const SphericalManifold<dim> boundary_description;
  parallel::distributed::Triangulation<dim> triangulation;
  DoFHandler<dim> dof_handler;

  IndexSet locally_owned_dofs;
  IndexSet locally_relevant_dofs;

  std::map<std::string, double> parser_constants;
  FunctionParser<dim> convection_function;
  FunctionParser<dim> forcing_function;

  ConstraintMatrix constraints;

  TrilinosWrappers::MPI::Vector locally_relevant_solution;
  TrilinosWrappers::MPI::Vector system_rhs;
  TrilinosWrappers::SparseMatrix system_matrix;

  TrilinosWrappers::PreconditionAMG preconditioner;

  void setup_geometry();
  void setup_matrices();
  void time_iterate();
};


template<int dim>
CDRProblem<dim>::CDRProblem(const CDR::Parameters &parameters) :
  parameters(parameters),
  time_step {(parameters.stop_time - parameters.start_time)
    /parameters.n_time_steps},
  mpi_communicator (MPI_COMM_WORLD),
  n_mpi_processes {Utilities::MPI::n_mpi_processes(mpi_communicator)},
  this_mpi_process {Utilities::MPI::this_mpi_process(mpi_communicator)},
  fe(parameters.fe_order),
  quad(3*(2 + parameters.fe_order)/2),
  boundary_description(Point<dim>(true)),
  triangulation(mpi_communicator, typename Triangulation<dim>::MeshSmoothing
                (Triangulation<dim>::smoothing_on_refinement |
                 Triangulation<dim>::smoothing_on_coarsening)),
  convection_function(dim),
  forcing_function(1)
{
  Assert(dim == 2, ExcNotImplemented());
  parser_constants["pi"] = numbers::PI;
  std::vector<std::string> convection_field
  {
    parameters.convection_field.substr
      (0, parameters.convection_field.find_first_of(",")),
    parameters.convection_field.substr
      (parameters.convection_field.find_first_of(",") + 1)
  };

  convection_function.initialize(std::string("x,y"), convection_field,
                                 parser_constants,
                                 /*time_dependent=*/false);
  forcing_function.initialize(std::string("x,y,t"), parameters.forcing,
                              parser_constants,
                              /*time_dependent=*/true);
  forcing_function.set_time(parameters.start_time);
}


template<int dim>
void CDRProblem<dim>::setup_geometry()
{
  const Point<dim> center(true);
  GridGenerator::hyper_shell(triangulation, center, parameters.inner_radius,
                             parameters.outer_radius);
  triangulation.set_manifold(manifold_id, boundary_description);
  for (const auto &cell : triangulation.active_cell_iterators())
    {
      cell->set_all_manifold_ids(0);
    }
  triangulation.refine_global(parameters.refinement_level);
  dof_handler.initialize(triangulation, fe);
  locally_owned_dofs = dof_handler.locally_owned_dofs();
  DoFTools::extract_locally_relevant_dofs(dof_handler, locally_relevant_dofs);

  locally_relevant_solution.reinit(locally_relevant_dofs, mpi_communicator);
}


template<int dim>
void CDRProblem<dim>::setup_matrices()
{
  constraints.clear();
  constraints.reinit(locally_relevant_dofs);
  DoFTools::make_hanging_node_constraints(dof_handler, constraints);
  VectorTools::interpolate_boundary_values(dof_handler, 0, ZeroFunction<dim>(),
                                           constraints);
  constraints.close();

  DynamicSparsityPattern dynamic_sparsity_pattern(locally_relevant_dofs);
  DoFTools::make_sparsity_pattern(dof_handler, dynamic_sparsity_pattern,
                                  constraints, false);
  SparsityTools::distribute_sparsity_pattern
    (dynamic_sparsity_pattern, dof_handler.n_locally_owned_dofs_per_processor(),
     mpi_communicator, locally_relevant_dofs);

  system_rhs.reinit(locally_owned_dofs, mpi_communicator);
  system_matrix.reinit(locally_owned_dofs, dynamic_sparsity_pattern,
                       mpi_communicator);
  CDR::create_system_matrix(dof_handler, quad, convection_function, parameters,
                            time_step, constraints, system_matrix);
  system_matrix.compress(VectorOperation::add);
  preconditioner.initialize(system_matrix);
}


template<int dim>
void CDRProblem<dim>::time_iterate()
{
  TrilinosWrappers::MPI::Vector completely_distributed_solution
    (locally_owned_dofs, mpi_communicator);

  double current_time = parameters.start_time;
  for (unsigned int time_step_n = 0; time_step_n < parameters.n_time_steps;
       ++time_step_n)
    {

      current_time += time_step;
      forcing_function.advance_time(time_step);

      system_rhs = 0.0;
      CDR::create_system_rhs(dof_handler, quad, convection_function,
                             forcing_function, parameters,
                             locally_relevant_solution, constraints, system_rhs);
      system_rhs.compress(VectorOperation::add);

      SolverControl solver_control(dof_handler.n_dofs(),
                                   1e-6*system_rhs.l2_norm());
      TrilinosWrappers::SolverGMRES solver(solver_control, mpi_communicator);
      solver.solve(system_matrix, completely_distributed_solution, system_rhs,
                   preconditioner);
      constraints.distribute(completely_distributed_solution);
      locally_relevant_solution = completely_distributed_solution;

      if (time_step_n % parameters.save_interval == 0)
        {
          DataOut<dim> data_out;
          data_out.attach_dof_handler(dof_handler);
          data_out.add_data_vector(locally_relevant_solution, "u");

          Vector<float> subdomain (triangulation.n_active_cells());
          for (unsigned int i = 0; i < subdomain.size(); ++i)
            {
              subdomain[i] = triangulation.locally_owned_subdomain();
            }
          data_out.add_data_vector(subdomain, "subdomain");
          data_out.build_patches();

          const std::string filename
          {"solution-" + Utilities::int_to_string(time_step_n) + "."
              + Utilities::int_to_string(triangulation.locally_owned_subdomain(), 4)};
          std::ofstream output((filename + ".vtu").c_str());
          data_out.write_vtu (output);

          if (this_mpi_process == 0)
            {
              std::vector<std::string> filenames;
              for (unsigned int i = 0; i < n_mpi_processes; ++i)
                filenames.push_back
                  ("solution-" + Utilities::int_to_string (time_step_n) + "."
                   + Utilities::int_to_string (i, 4) + ".vtu");
              std::ofstream master_output
                ("solution-" + Utilities::int_to_string(time_step_n) + ".pvtu");
              data_out.write_pvtu_record(master_output, filenames);
            }
        }
    }
}


template<int dim>
void CDRProblem<dim>::run()
{
  setup_geometry();
  setup_matrices();
  time_iterate();
}


constexpr int dim {2};


int main(int argc, char *argv[])
{
  Utilities::MPI::MPI_InitFinalize mpi_initialization
    (argc, argv, numbers::invalid_unsigned_int);
  CDR::Parameters parameters
  {
    1.0, 2.0,
    1.0e-3, "-y,x", 1.0e-4, "exp(-2*t)*exp(-40*(x - 1.5)^6)"
    "*exp(-40*y^6)", true,
    3, 2,
    0.0, 2.0, 200,
    1, 3
  };
  CDRProblem<dim> cdr_problem(parameters);
  cdr_problem.run();

  return 0;
}