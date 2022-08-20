
#include <iostream>
#include <iomanip>
#include <cstdlib>
#include <cstdio>
#include <vector>
#include <cstring>
#include <array>
#include <tuple>

#include <mpi.h>
#include <fftw3-mpi.h>

#include "pfc.h"

#include <random>

// ---------------------------------------------------------------
// PARAMETERS
//
// initialization could also be done in header, if const->constexpr 

const int PhaseField::nx = 512;      //grid size in x direction
const int PhaseField::ny = 512;      //grid size in y direction

const double PhaseField::dx = 0.25;  //space step in x dir.
const double PhaseField::dy = 0.25;  //space step in x dir.
const double PhaseField::dt = 0.125; //time step

//one mode approximation lowest order reciprocal lattice vectors
//2D hexagonal crystal symmetry. Eq.(2.4)
const double PhaseField::q_vec[][2] = 
    {{-0.5*sq3, -0.5},
     {0.0, 1.0},
     {0.5*sq3, -0.5}};

const int    PhaseField::nparticles = 5; // number of particles
const double PhaseField::particle_radius = 0.15; // (max)
const double PhaseField::angle = 3.1415926/180*20.0; // (max) the grain rotation angle [rad] (e.g., 5 [degree])
const double PhaseField::amplitude = 0.10867304595992146;//the perfect lattice equilibrium value

const double PhaseField::bx = 1.0;  //B^x in Eq.(2.7)
const double PhaseField::bl = 0.95; //B^l = B^x - dB
const double PhaseField::tt = 0.585;//tau * - (phi^3)/3, Eq.(2.1) and Eq.(2.2)
const double PhaseField::vv = 1.0;  //nu  * (phi^4)/4,, Eq.(2.2)

const int    PhaseField::out_time = 80;
const int    PhaseField::max_iterations = 5000; 

const int    PhaseField::nc = 3;

// ---------------------------------------------------------------

PhaseField::PhaseField(int mpi_rank_, int mpi_size_, std::string output_path_)
        : mpi_rank(mpi_rank_), mpi_size(mpi_size_), output_path(output_path_), mech_eq(this) {

    fftw_mpi_init();
   
    // Allocate and calculate k values
    k_x_values = (double*) malloc(sizeof(double)*nx);
    k_y_values = (double*) malloc(sizeof(double)*ny);
    calculate_k_values(k_x_values, nx, dx);
    calculate_k_values(k_y_values, ny, dy);
    
    // Allocate etas and FFT plans and same for buffer values
    eta = (complex<double>**) malloc(sizeof(complex<double>*)*nc);
    eta_k = (complex<double>**) malloc(sizeof(complex<double>*)*nc);
    eta_plan_f = (fftw_plan*) malloc(sizeof(fftw_plan)*nc);
    eta_plan_b = (fftw_plan*) malloc(sizeof(fftw_plan)*nc);

    eta_tmp = (complex<double>**) malloc(sizeof(complex<double>*)*nc);
    eta_tmp_k = (complex<double>**) malloc(sizeof(complex<double>*)*nc);
    eta_tmp_plan_f = (fftw_plan*) malloc(sizeof(fftw_plan)*nc);
    eta_tmp_plan_b = (fftw_plan*) malloc(sizeof(fftw_plan)*nc);

    buffer = (complex<double>**) malloc(sizeof(complex<double>*)*nc);
    buffer_k = (complex<double>**) malloc(sizeof(complex<double>*)*nc);
    buffer_plan_f = (fftw_plan*) malloc(sizeof(fftw_plan)*nc);
    buffer_plan_b = (fftw_plan*) malloc(sizeof(fftw_plan)*nc);

	exp_part = (complex<double>**) malloc(sizeof(complex<double>*)*nc);

    alloc_local = fftw_mpi_local_size_2d(nx, ny, MPI_COMM_WORLD,
            &local_nx, &local_nx_start);

    // Allocate memory for G_j values and theta gradient
    g_values = (double**) malloc(sizeof(double*)*nc);
    grad_theta = (double**) malloc(sizeof(double*)*nc);
    for (int c = 0; c < nc; c++) {
        g_values[c] = (double*) malloc(sizeof(double)*local_nx*ny);
        grad_theta[c] = (double*) malloc(sizeof(double)*local_nx*ny);
    }
    calculate_g_values(g_values);


    for (int i = 0; i < nc; i++) {
        eta[i] = reinterpret_cast<complex<double>*>(fftw_alloc_complex(alloc_local));
        eta_k[i] = reinterpret_cast<complex<double>*>(fftw_alloc_complex(alloc_local));
        eta_plan_f[i] = fftw_mpi_plan_dft_2d(nx, ny,
                reinterpret_cast<fftw_complex*>(eta[i]),
                reinterpret_cast<fftw_complex*>(eta_k[i]),
                MPI_COMM_WORLD, FFTW_FORWARD, FFTW_ESTIMATE);
        eta_plan_b[i] = fftw_mpi_plan_dft_2d(nx, ny,
                reinterpret_cast<fftw_complex*>(eta_k[i]),
                reinterpret_cast<fftw_complex*>(eta[i]),
                MPI_COMM_WORLD, FFTW_BACKWARD, FFTW_ESTIMATE);

        eta_tmp[i] = reinterpret_cast<complex<double>*>(fftw_alloc_complex(alloc_local));
        eta_tmp_k[i] = reinterpret_cast<complex<double>*>(fftw_alloc_complex(alloc_local));
        eta_tmp_plan_f[i] = fftw_mpi_plan_dft_2d(nx, ny,
                reinterpret_cast<fftw_complex*>(eta_tmp[i]),
                reinterpret_cast<fftw_complex*>(eta_tmp_k[i]),
                MPI_COMM_WORLD, FFTW_FORWARD, FFTW_ESTIMATE);
        eta_tmp_plan_b[i] = fftw_mpi_plan_dft_2d(nx, ny,
                reinterpret_cast<fftw_complex*>(eta_tmp_k[i]),
                reinterpret_cast<fftw_complex*>(eta_tmp[i]),
                MPI_COMM_WORLD, FFTW_BACKWARD, FFTW_ESTIMATE);

        buffer[i] = reinterpret_cast<complex<double>*>(fftw_alloc_complex(alloc_local));
        buffer_k[i] = reinterpret_cast<complex<double>*>(fftw_alloc_complex(alloc_local));
        buffer_plan_f[i] = fftw_mpi_plan_dft_2d(nx, ny,
                reinterpret_cast<fftw_complex*>(buffer[i]),
                reinterpret_cast<fftw_complex*>(buffer_k[i]),
                MPI_COMM_WORLD, FFTW_FORWARD, FFTW_ESTIMATE);
        buffer_plan_b[i] = fftw_mpi_plan_dft_2d(nx, ny,
                reinterpret_cast<fftw_complex*>(buffer_k[i]),
                reinterpret_cast<fftw_complex*>(buffer[i]),
                MPI_COMM_WORLD, FFTW_BACKWARD, FFTW_ESTIMATE);
    	
    	exp_part[i] = reinterpret_cast<complex<double>*>(fftw_alloc_complex(alloc_local));
    }

}

PhaseField::~PhaseField() {
    for (int i = 0; i < nc; i++) {
        fftw_free(eta[i]); fftw_free(eta_k[i]);
        fftw_destroy_plan(eta_plan_f[i]); fftw_destroy_plan(eta_plan_b[i]);

        fftw_free(buffer[i]); fftw_free(buffer_k[i]);
        fftw_destroy_plan(buffer_plan_f[i]); fftw_destroy_plan(buffer_plan_b[i]);

        free(g_values[i]);
    }
    free(eta); free(eta_k);
    free(eta_plan_f); free(eta_plan_b);

    free(buffer); free(buffer_k);
    free(buffer_plan_f); free(buffer_plan_b);

    free(k_x_values); free(k_y_values);
    free(g_values);
	
	free(exp_part);
}

/*! Method, that initializes the state to a elastically rotated circle
 *
 */
void PhaseField::initialize_eta_circle() {
    //double angle = 0.0872665;
    //double amplitude = 0.10867304595992146;

    for (int i = 0; i < local_nx; i++) {
        int i_gl = i + local_nx_start;
        for (int j = 0; j < ny; j++) {
            for (int c = 0; c < nc; c++) {
                if ((i_gl+1-nx/2.0)*(i_gl+1-nx/2.0)*dx*dx + (j+1-ny/2.0)*(j+1-ny/2.0)*dy*dy
                        <= (0.25*nx*dx)*(0.25*nx*dx)) {
                    // Inside the rotated circle
                    double theta = (q_vec[c][0]*cos(angle) + q_vec[c][1]*sin(angle)
                                    - q_vec[c][0])*(i_gl+1-nx/2.0)*dx
                                 + (-q_vec[c][0]*sin(angle) + q_vec[c][1]*cos(angle)
                                    - q_vec[c][1])*(j+1-ny/2.0)*dy;
                    eta[c][i*ny + j] = amplitude*exp(complex<double>(0.0, 1.0)*theta);
                } else {
                    // Outside the circle
                    eta[c][i*ny + j] = amplitude;
                }
            }
        }
    }
}


/*! Method, that initializes the state of eta to liquid with a seed in center
 *
 */
void PhaseField::initialize_eta_seed() {
    //double amplitude = 0.10867304595992146;
    double seed_radius = 0.05*nx*dx;

    for (int i = 0; i < local_nx; i++) {
        int i_gl = i + local_nx_start;
        for (int j = 0; j < ny; j++) {
            for (int c = 0; c < nc; c++) {
                double center_dist = sqrt((i_gl+1-nx/2.0)*(i_gl+1-nx/2.0)*dx*dx
                                     + (j+1-ny/2.0)*(j+1-ny/2.0)*dy*dy);
                double rd = center_dist/seed_radius;
                eta[c][i*ny + j] = amplitude/(rd*rd*rd*rd+1);
            }
        }
    }
}

/*! Method, that initializes the state of eta to liquid with a seed in center
 *
 */
void PhaseField::initialize_eta_multiple_seeds() {
    //double amplitude = 0.10867304595992146;

    // <rel_x, rel_y, size, angle>
    /*
    std::vector<std::tuple<double, double, double, double>> seeds = {
    		std::make_tuple(0.86, 0.66, 0.06, -0.20),
			std::make_tuple(0.59, 0.21, 0.03, -0.10),
			std::make_tuple(0.33, 0.80, 0.06,  0.00),
			std::make_tuple(0.38, 0.41, 0.06,  0.05),
			std::make_tuple(0.51, 0.17, 0.03,  0.20),
			std::make_tuple(0.16, 0.19, 0.03,  0.10),
			std::make_tuple(0.99, 0.99, 0.12,  0.15)
    };
    */

    //std::vector<std::tuple<double, double, double, double>> seeds = {
	//		std::make_tuple(0.3, 0.5, 0.15, 0.0),
	//		std::make_tuple(0.7, 0.5, 0.15, 0.2),
    //};
	
	std::srand(std::time(nullptr));
    std::vector<std::tuple<double, double, double, double>> seeds;
	for (int i = 0; i < nparticles; i++) {
		seeds.push_back( std::make_tuple( 
			(double) rand()/RAND_MAX, 
			(double) rand()/RAND_MAX,
			(double) rand()/RAND_MAX*particle_radius, 
			(double) rand()/RAND_MAX*angle
		) );
	}


    for (int i = 0; i < local_nx; i++) {
        int i_gl = i + local_nx_start;
        for (int j = 0; j < ny; j++) {
            for (int c = 0; c < nc; c++) {
            	eta[c][i*ny + j] = 0.0;
            	for (auto seed : seeds) {
            		// current coordinates
            		double x = i_gl*dx;
            		double y = j*dy;

            		// seed center coordinates
            		double x_c = (nx-1)*std::get<0>(seed)*dx;
            		double y_c = (ny-1)*std::get<1>(seed)*dy;

            		// differences considering periodic boundary
            		double x_dif = (abs(x-x_c) < nx*dx-abs(x-x_c)) ? (x-x_c) : (nx*dx-(x-x_c));
            		double y_dif = (abs(y-y_c) < ny*dy-abs(y-y_c)) ? (y-y_c) : (ny*dy-(y-y_c));

            		if (abs(x-x_c) < nx*dx-abs(x-x_c)) x_dif = x-x_c;
            		else if (abs((x+nx*dx)-x_c) < abs((x-nx*dx)-x_c)) x_dif = (x+nx*dx)-x_c;
            		else x_dif = (x-nx*dx)-x_c;

            		if (abs(y-y_c) < ny*dy-abs(y-y_c)) y_dif = y-y_c;
					else if (abs((y+ny*dy)-y_c) < abs((y-ny*dy)-y_c)) y_dif = (y+ny*dy)-y_c;
					else y_dif = (y-ny*dy)-y_c;

            		// distance from center (considering periodic boundary)
            		double center_dist = std::sqrt(x_dif*x_dif + y_dif*y_dif);
            		double rd = center_dist/(std::get<2>(seed)*nx*dx);

            		// apply a rotation around point (x_c, y_c) by the angle
            		double ang = std::get<3>(seed);

            		double theta = q_vec[c][0]*((cos(ang)-1)*x_dif - sin(ang)*y_dif)
								   + q_vec[c][1]*(sin(ang)*x_dif + (cos(ang)-1)*y_dif);

                    eta[c][i*ny + j] += amplitude/(std::pow(rd, 16)+1)
                    					* exp(complex<double>(0.0, 1.0)*theta);
            	}
            }
        }
    }
}


void PhaseField::take_fft(fftw_plan *plan) {
    for (int i = 0; i < nc; i++) {
        fftw_execute(plan[i]);
    }
}

void PhaseField::normalize_field(complex<double> **field) {
    double scale = 1.0/(nx*ny);
    for (int i = 0; i < local_nx; i++) {
        for (int j = 0; j < ny; j++) {
            for (int n = 0; n < nc; n++) {
                field[n][i*ny + j] *= scale;
            }
        }
    }
}

complex<double>* PhaseField::get_eta(int num) {
    return eta[num];
}

complex<double>* PhaseField::get_eta_k(int num) {
    return eta_k[num];
}

/*! Method that gathers all eta or eta_k data to root process and prints it out
 *  
 *  NB: The whole data must fit inside root process memory
 */
void PhaseField::output_field(complex<double>*field) {

    complex<double> *field_total = (complex<double>*)
            fftw_malloc(sizeof(complex<double>)*nx*ny);

    // local_nx*ny*2, because one fftw_complex element contains 2 doubles
    MPI_Gather(field, local_nx*ny*2, MPI_DOUBLE, field_total, local_nx*ny*2, MPI_DOUBLE, 0, MPI_COMM_WORLD);
    if (mpi_rank == 0) {
        for (int i = 0; i < nx; i++) {
            std::cout << "|";
            for (int j = 0; j < ny; j++) {
                printf("%11.4e ", real(field_total[i*ny+j]));
            }
            std::cout << "| |";
    
            for (int j = 0; j < ny; j++) {
                printf("%11.4e ", imag(field_total[i*ny+j]));
            }
            std::cout << "|" << std::endl;
        }
    
        std::cout << std::endl;
    }
    fftw_free(field_total);
}

/*! Method to calculate the wave number values corresponding to bins in k space
 *
 *  The method was written on basis of numpy.fft.fftfreq() 
 */
void PhaseField::calculate_k_values(double *k_values, int n, double d) {
    for (int i = 0; i < n; i++) {
        if (n % 2 == 0) {
        // If n is even
            if (i < n/2) k_values[i] = 2*PI*i/(n*d);
            else k_values[i] = 2*PI*(i-n)/(n*d);
        } else {
        // Else n is odd
            if (i <= n/2) k_values[i] = 2*PI*i/(n*d);
            else k_values[i] = 2*PI*(i-n)/(n*d);
        }
    }
}

/*! Method that calculates G_j values in k space
 *  
 */
void PhaseField::calculate_g_values(double **g_values) {
    for (int i = 0; i < local_nx; i++) {
        int i_shift = i + local_nx_start;
        for (int j = 0; j < ny; j++) {
            double k_sq = k_x_values[i_shift]*k_x_values[i_shift]
                + k_y_values[j]*k_y_values[j];
            for (int n = 0; n < 3; n++) {
                g_values[n][i*ny + j] = - k_sq - 2*(q_vec[n][0]*k_x_values[i_shift]
                        + q_vec[n][1]*k_y_values[j]);
            }
        }
    }
}


void PhaseField::memcopy_eta(complex<double> **eta_to, complex<double> **eta_from) {
    for (int c = 0; c < nc; c++) {
        std::memcpy(eta_to[c], eta_from[c], sizeof(complex<double>)*local_nx*ny);
    }
}


/*! Method to calculate energy.
 *
 *  NB: This method assumes that eta_k is set beforehand.
 *  Takes 1 fft
 */
double PhaseField::calculate_energy(complex<double> **eta_, complex<double> **eta_k_) {
    // will use the member variable buffer_k to hold (G_j eta_j)_k
    memcopy_eta(buffer_k, eta_k_);

    //  Multiply eta_k by G_j in k space
    for (int c = 0; c < nc; c++) {
        for (int i = 0; i < local_nx; i++) {
            for (int j = 0; j < ny; j++) {
                buffer_k[c][i*ny + j] *= g_values[c][i*ny + j];
            }
        }
    }

    // Go to real space for (G_j eta_j)
    take_fft(buffer_plan_b);
    normalize_field(buffer);

    // Integrate the whole expression over space and divide by num cells to get density
    // NB: this will be the contribution from local MPI process only
    double local_energy = 0.0;
    for (int i = 0; i < local_nx; i++) {
        for (int j = 0; j < ny; j++) {
            complex<double> eta0=eta_[0][i*ny+j]; complex<double> buf0=buffer[0][i*ny+j];
            complex<double> eta1=eta_[1][i*ny+j]; complex<double> buf1=buffer[1][i*ny+j];
            complex<double> eta2=eta_[2][i*ny+j]; complex<double> buf2=buffer[2][i*ny+j];
            double aa = 2*(abs(eta0)*abs(eta0) + abs(eta1)*abs(eta1) + abs(eta2)*abs(eta2));

            local_energy += aa*(bl-bx)/2.0 + (3.0/4.0)*vv*aa*aa
                         - 4*tt*real(eta0*eta1*eta2)
                         + bx*(abs(buf0)*abs(buf0)+abs(buf1)*abs(buf1)+abs(buf2)*abs(buf2))
                         - (3.0/2.0)*vv*(abs(eta0)*abs(eta0)*abs(eta0)*abs(eta0)
                                            + abs(eta1)*abs(eta1)*abs(eta1)*abs(eta1)
                                            + abs(eta2)*abs(eta2)*abs(eta2)*abs(eta2));
        }
    }
    local_energy *= 1.0/(nx*ny);

    double energy = 0.0;
    
    MPI_Allreduce(&local_energy, &energy, 1, MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD);
    /*
    if (mpi_rank == 0) {
        printf("Energy: %.16e\n", energy);
    }
    */
    return energy;
}

/*! Method, that calculates the three components of the nonlinear part
 *
 *  The components will be saved to components (memory must be allocated before)
 *  and they correspond to real space indices (coordinates) of i, j
 */
void PhaseField::calculate_nonlinear_part(int i, int j, complex<double> *components,
        complex<double> **eta_) {
    complex<double> eta0 = eta_[0][i*ny+j];
    complex<double> eta1 = eta_[1][i*ny+j];
    complex<double> eta2 = eta_[2][i*ny+j];
    double aa = 2*(abs(eta0)*abs(eta0) + abs(eta1)*abs(eta1) + abs(eta2)*abs(eta2));
    components[0] = 3*vv*(aa-abs(eta0)*abs(eta0))*eta0 - 2*tt*conj(eta1)*conj(eta2);
    components[1] = 3*vv*(aa-abs(eta1)*abs(eta1))*eta1 - 2*tt*conj(eta0)*conj(eta2);
    components[2] = 3*vv*(aa-abs(eta2)*abs(eta2))*eta2 - 2*tt*conj(eta1)*conj(eta0);
}

/*! Method, which takes an overdamped dynamics time step
 *
 */
void PhaseField::overdamped_time_step() {
    // Will use buffer to hold intermediate results
    memcopy_eta(buffer, eta);

    // allocate memory for the nonlinear part
    complex<double> *nonlinear_part = (complex<double>*) malloc(sizeof(complex<double>)*nc);

    // buffer contains eta, let's subtract dt*(nonlinear part)
    // to get the numerator in the OD time stepping scheme (in real space)
    for (int i = 0; i < local_nx; i++) {
        for (int j = 0; j < ny; j++) {
            calculate_nonlinear_part(i, j, nonlinear_part, eta);
            for (int c = 0; c < nc; c++) {
                buffer[c][i*ny + j] -= dt*nonlinear_part[c];
            }
        }
    }
    free(nonlinear_part);
    
    // take buffer into k space
    take_fft(buffer_plan_f);
    
    // now eta_k can be evaluated correspondingly to the scheme
    for (int c = 0; c < nc; c++) {
		for (int i = 0; i < local_nx; i++) {
			for (int j = 0; j < ny; j++) {
				eta_k[c][i*ny + j] = buffer_k[c][i*ny + j] / (1.0 + dt*(bl-bx
										+bx*g_values[c][i*ny + j]*g_values[c][i*ny + j]));
            }
        }
    }

    // Take eta back to real space
    take_fft(eta_plan_b);
    normalize_field(eta);

}

double PhaseField::dot_prod(const double* v1, const double* v2, const int len) {
    double res = 0.0;
    for (int i = 0; i < len; i++) {
        res += v1[i]*v2[i];
    }
    return res;
}

/*! Method, which calculates the gradient of thetas
 *
 *  The resulting gradient will be stored in "grad_theta"
 *  NB: required eta_k to be set
 *  Takes 1 fft
 */
void PhaseField::calculate_grad_theta(complex<double> **eta_, complex<double> **eta_k_) {
    // will use the member variable buffer_k to hold (G_j^2 eta_j)_k
    memcopy_eta(buffer_k, eta_k_);

    //  Multiply eta_k by G_j^2 in k space
    for (int c = 0; c < nc; c++) {
        for (int i = 0; i < local_nx; i++) {
            for (int j = 0; j < ny; j++) {
                buffer_k[c][i*ny + j] *= g_values[c][i*ny + j]*g_values[c][i*ny + j];
            }
        }
    }

    // Go to real space for (G_j^2 eta_j)
    take_fft(buffer_plan_b);
    normalize_field(buffer);

    // allocate memory for the nonlinear part and a temporary result
    complex<double> *nonlinear_part = (complex<double>*) malloc(sizeof(complex<double>)*nc);
    double *im = (double*) malloc(sizeof(double)*nc);

    for (int i = 0; i < local_nx; i++) {
        for (int j = 0; j < ny; j++) {
           calculate_nonlinear_part(i, j, nonlinear_part, eta_);
           for (int c = 0; c < nc; c++) {
                complex<double> var_f_eta = (bl-bx)*eta_[c][i*ny+j] + bx*buffer[c][i*ny+j]
                                            + nonlinear_part[c];
                im[c] = imag(conj(eta_[c][i*ny+j])*var_f_eta);
           }
           for (int c = 0; c < nc; c++) {
                double dtheta = dot_prod(q_vec[c], q_vec[0], 2)*im[0]
                               + dot_prod(q_vec[c], q_vec[1], 2)*im[1]
                               + dot_prod(q_vec[c], q_vec[2], 2)*im[2];
                grad_theta[c][i*ny+j] = dtheta;
           }
        }
    }

    free(nonlinear_part);
    free(im);
}

/*! Method, that writes current eta to a binary file
 *
 *  The data is structured as follows:
 *  8 byte doubles, alternating real and imaginary parts,
 *  if eta[c][i*ny+j], then fastest moving index is j, then i and finally c
 */
void PhaseField::write_eta_to_file(string filepath) {


    // delete old file
    if (mpi_rank == 0) MPI_File_delete(filepath.c_str(), MPI_INFO_NULL);

    MPI_File mpi_file;
    int rcode = MPI_File_open(MPI_COMM_WORLD, filepath.c_str(),
            MPI_MODE_CREATE | MPI_MODE_RDWR, MPI_INFO_NULL, &mpi_file);

    if (rcode != MPI_SUCCESS)
        cerr << "Error: couldn't open file" << endl;
    for (int c = 0; c < nc; c++) {
        MPI_Offset offset = local_nx_start*ny*sizeof(double)*2 + c*nx*ny*sizeof(double)*2;
        rcode = MPI_File_set_view(mpi_file, offset,
                                    MPI_DOUBLE, MPI_DOUBLE, "native", MPI_INFO_NULL);
    
        if(rcode != MPI_SUCCESS)
            cerr << "Error: couldn't set file process view" << endl;
    
        rcode = MPI_File_write(mpi_file, eta[c], local_nx*ny*2,
                    MPI_DOUBLE, MPI_STATUS_IGNORE);
    
        if(rcode != MPI_SUCCESS)
            cerr << "Error: couldn't write file" << endl;
    }

    MPI_File_close(&mpi_file);
}

void PhaseField::write_eta_to_vtk_file(string filepath) {

	FILE *fp;
	fp = fopen(filepath.c_str(), "w");
	fprintf(fp,"# vtk DataFile Version 3.0 \n");
	fprintf(fp,"output.vtk \n");
	fprintf(fp,"ASCII \n");
	fprintf(fp,"DATASET STRUCTURED_POINTS \n");
	fprintf(fp,"DIMENSIONS %5d %5d %5d \n",(nx),(ny),(1));
	fprintf(fp,"ORIGIN 0.0 0.0 0.0 \n");
	fprintf(fp,"ASPECT_RATIO %f %f %f \n",float(nx/nx),float(ny/nx),float(1));
	fprintf(fp,"POINT_DATA %16d \n",((nx)*(ny)*(1)));
	fprintf(fp,"SCALARS eta float \n");
	fprintf(fp,"LOOKUP_TABLE default \n");
	for(int j=0;j<ny;j++){
		for(int i=0;i<nx;i++){
			fprintf(fp,"%10.6f\n", (abs(eta[0][i*ny+j])+abs(eta[1][i*ny+j])+abs(eta[2][i*ny+j])));
		}
	}
	fprintf(fp,"SCALARS phi float \n");
	fprintf(fp,"LOOKUP_TABLE default \n");
	for(int j=0;j<ny;j++){
		for(int i=0;i<nx;i++){
			fprintf(fp,"%10.6f\n",  (abs(eta[0][i*ny+j] *exp_part[0][i*ny+j]+conj(eta[0][i*ny+j]*exp_part[0][i*ny+j]))
									+abs(eta[1][i*ny+j] *exp_part[1][i*ny+j]+conj(eta[1][i*ny+j]*exp_part[1][i*ny+j]))
									+abs(eta[2][i*ny+j] *exp_part[2][i*ny+j]+conj(eta[2][i*ny+j]*exp_part[2][i*ny+j]))));
		}
	}
}

/*! Reads eta from a binary file written by write_eta_to_file
 *
 */
void PhaseField::read_eta_from_file(string filepath) {

    MPI_File mpi_file;
    int rcode = MPI_File_open(MPI_COMM_WORLD, filepath.c_str(), MPI_MODE_RDWR,
            MPI_INFO_NULL, &mpi_file);

    if (rcode != MPI_SUCCESS)
        cerr << "Error: couldn't open file" << endl;

    for (int c = 0; c < nc; c++) {
        MPI_Offset offset = local_nx_start*ny*sizeof(double)*2 + c*nx*ny*sizeof(double)*2;
        rcode = MPI_File_set_view(mpi_file, offset,
                                    MPI_DOUBLE, MPI_DOUBLE, "native", MPI_INFO_NULL);
    
        if(rcode != MPI_SUCCESS)
            cerr << "Error: couldn't set file process view" << endl;
    
        rcode = MPI_File_read(mpi_file, eta[c], local_nx*ny*2,
                    MPI_DOUBLE, MPI_STATUS_IGNORE);

        if(rcode != MPI_SUCCESS)
            cerr << "Error: couldn't read file" << endl;
    }
    MPI_File_close(&mpi_file);
}


double PhaseField::calculate_radius() {
    int line_x = nx/2 - local_nx_start;

    int argmin1 = 0, argmin2 = 0;
    double phi_min = 0.0;

    if (line_x >= 0 && line_x < local_nx) {
        for (int j = 0; j < ny; j++) {
            double phi = 0.0;
            for (int c = 0; c < nc; c++)
                phi += abs(eta[c][line_x*ny + j]);
            if (j == 0 || j == ny/2)
                phi_min = phi;
            // if in first half
            if (j < ny/2) {
                if (phi <= phi_min) {
                    phi_min = phi;
                    argmin1 = j;
                }           
            } else {
                if (phi <= phi_min) {
                    phi_min = phi;
                    argmin2 = j;
                }           
            }
        }
    }
    double radius = abs(argmin2 - argmin1)*dy; 
    MPI_Allreduce(&radius, &radius, 1, MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD);

    return radius;
}

/*! Method, does the initial setup to run the circle calculations from scratch
 *
 */
void PhaseField::start_calculations() {

    string path = output_path + "seed_run/";
    string run_info_filename = "run_info.txt";

    // check if program can find the path
    if (mpi_rank == 0) {
		FILE * run_info_file = fopen((path+run_info_filename).c_str(), "w");
		if (run_info_file == NULL) {
			std::cout << "Can't access " << (path+run_info_filename) << std::endl;
			MPI_Abort(MPI_COMM_WORLD, 1);
		}
		fclose(run_info_file);
    }

    // initialize eta and eta_k
    initialize_eta_multiple_seeds();
    take_fft(eta_plan_f);

    // write initial conf to file
	write_eta_to_file(path+"initial_conf.bin");

    double energy = calculate_energy(eta, eta_k);
    if (mpi_rank == 0)
        printf("Initial state - energy: %.16e\n", energy);

    run_calculations(0, 0.0, path, run_info_filename);
}


void PhaseField::run_calculations(int init_it, double time_so_far,
        string path, string run_info_filename) {

    Time::time_point time_start = Time::now();
    Time::time_point time_var = Time::now();

    // Start repetitions of overdamped steps and mechanical equilibrium
    int repetitions = 50000;
    int od_steps = 80;

    int save_freq = 5;
    FILE * run_info_file;

    int ts = init_it; // total over-damped timesteps counter

    for (int rep = 1; rep < repetitions; rep++) {
        time_var = Time::now();
        // Over-damped timesteps
        for (int ts_ = 0; ts_ < od_steps; ts_++) {
            overdamped_time_step();
        }
        ts += od_steps;
        double od_dur = std::chrono::duration<double>(Time::now()-time_var).count();
        // Mechanical equilibration
        int meq_iter = mech_eq.lbfgs_enhanced();
        //int meq_iter = 0;
        double meq_dur = std::chrono::duration<double>(Time::now()-time_var).count()
                           - od_dur;
        double energy = calculate_energy(eta, eta_k);
        double total_dur = std::chrono::duration<double>(Time::now()-time_start).count()
                            + time_so_far;
        if (mpi_rank == 0) {
            // Print run information
            printf("ts: %5d; stime: %7.1f; energy: %.16e; od_time: %4.1f; "
                   "meq_iter: %d; meq_time: %5.1f; total_time: %7.1f\n",
				   ts, ts*dt, energy, od_dur,
                   meq_iter, meq_dur, total_dur);
            // Save run information also to a file
            run_info_file = fopen((path+run_info_filename).c_str(), "a");
            fprintf(run_info_file, "%d %.1f %.16e %.1f %d %.1f %.1f\n",
            		ts, ts*dt, energy, od_dur,
                    meq_iter, meq_dur, total_dur);
            fclose(run_info_file);
        }
	if ((rep-1)*od_steps*dt > 700 && save_freq < 20) {
	    save_freq = 100;
	}
        if (rep % save_freq == 0) {
            std::stringstream sstream;
            sstream << std::fixed << std::setprecision(0) << ts*dt;
            write_eta_to_file(path+"eta_"+sstream.str()+".bin");
        }
    }
}


void PhaseField::continue_calculations() {

    string path = output_path + "testrun/";
    string run_info_filename = "run_info.txt";

    string continue_from_file = "eta_10.bin";
    double continue_stime = 10.0;
    
    // Load eta
    read_eta_from_file(path+continue_from_file);
    if (mpi_rank == 0)
        cout << "Loaded eta from file: " << path+continue_from_file << endl;

    FILE * run_info_file = fopen((path+run_info_filename).c_str(), "r");

    std::vector< std::array<double, 7> > data;

    int init_it, meq_iter;
    double stime, en, odd, meqd, total_dur;
    // Read the data until the starting point from run_info_file
    while (fscanf(run_info_file, "%d %lf %lf %lf %d %lf %lf\n",
                    &init_it, &stime, &en, &odd, &meq_iter,
                    &meqd, &total_dur) != EOF) {
        std::array<double, 7> line{
            {double(init_it), stime, en, odd, double(meq_iter), meqd,
                total_dur}
        };
        data.push_back(line);
        if (abs(stime-continue_stime) < 0.1) break;
    }
    
    fclose(run_info_file);
    run_info_file = fopen((path+run_info_filename).c_str(), "w");
    // Overwrite the data with only the relevant part
    for (unsigned int ln = 0; ln < data.size(); ln++) {
        fprintf(run_info_file, "%d %.1f %.16e %.1f %d %.1f %d %.1f\n",
                int(data[ln][0]), data[ln][1], data[ln][2],
                data[ln][3], int(data[ln][4]),
                data[ln][5], int(data[ln][6]), data[ln][7]);
    }
    fclose(run_info_file);

    if (mpi_rank == 0)
        cout << "Read info from " << run_info_filename
             << " and cleaned redundant entries." << endl; 

    run_calculations(init_it, total_dur, path, run_info_filename);
}


void PhaseField::test() {
	initialize_eta_multiple_seeds();
	take_fft(eta_plan_f);
	//write_eta_to_file(output_path + "initial_conf.bin");

    //for (int it = 0; it < 400; it++) {
    //    overdamped_time_step();
    //}

    //write_eta_to_file(output_path + "eta50.bin");

    //for (int it = 0; it < 400; it++) {
    //	overdamped_time_step();
    //}

    //write_eta_to_file(output_path + "eta100.bin");

	double theta_phi;
	for(int i=0;i<nx;i++){
		for(int j=0;j<ny;j++){
			for(int c=0;c<nc;c++){
				theta_phi = ( q_vec[c][0] * (double)(i+1 - nx/2.0) * dx/2.0
							+ q_vec[c][1] * (double)(j+1 - ny/2.0) * dy/2.0 );
				exp_part[c][i*ny+j] = exp(complex<double>(0.0, 1.0)*theta_phi);
			}
		}
	}
	
    for (int it = 0; it < max_iterations; it++) {
        overdamped_time_step();
    	if((it % out_time) == 0) {
    		write_eta_to_file(output_path+"eta_"+to_string(it)+".bin");
    		write_eta_to_vtk_file(output_path+"eta_"+to_string(it)+".vtk");
    	}
    }

/*
	initialize_eta_multiple_seeds();
	take_fft(eta_plan_f);
	write_eta_to_file(output_path + "initial_conf.bin");
	
    for (int it = 0; it < 80; it++) {
        overdamped_time_step();
    }

    double energy = calculate_energy(eta, eta_k);
	if (mpi_rank == 0)
		printf("energy before mech. eq.: %.16e\n", energy);

    mech_eq.lbfgs_enhanced();
    
    energy = calculate_energy(eta, eta_k);
	if (mpi_rank == 0)
		printf("energy after mech. eq.: %.16e\n", energy);
*/
}


