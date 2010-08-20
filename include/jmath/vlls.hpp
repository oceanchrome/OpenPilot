/* $Id$ */

#ifndef JMATH_LINEAR_LEAST_SQUARES2_HPP
#define JMATH_LINEAR_LEAST_SQUARES2_HPP

#include "jafarConfig.h"

#ifdef HAVE_BOOST_SANDBOX
#ifdef HAVE_LAPACK

#include "jmath/jblas.hpp"

namespace jafar {
  namespace jmath {

    /** Linear Least Squares solver. Find \f$x\f$ which minimizes:
     * \f[
     * \left\| A.x + b \right\|^2
     * \f]
     * 
     * The difference with \ref LinearLeastSquares is that the size of
     * the data sets is not staticly set, which means you can reduce the
     * size of the data sets, or add new points and do a new computation
     * without reinitializing the solver.
     */

    class VariableSizeLinearLeastSquares2 {

    private:

      std::size_t m_modelSize;
      int m_countValues;

      /// design matrix
      jblas::mat_column_major m_A;

      /// rhs vector
      jblas::vec m_b;

      /// least squares solution
      jblas::vec m_x;

      /// least squares solution covariance
      jblas::sym_mat m_xCov;

    public:
      
			VariableSizeLinearLeastSquares2(size_t _modelSize);
      std::size_t modelSize() const {return m_modelSize;}

      jblas::mat_column_major const& A() const {return m_A;}
      jblas::mat_column_major& A() {return m_A;}

      jblas::vec const& b() const {return m_b;}
      jblas::vec& b() {return m_b;}

      jblas::vec const& x() const {return m_x;}
      jblas::sym_mat const& xCov() const {return m_xCov;}

      void setSize(std::size_t sizeModel, std::size_t sizeDataSet);
      void setDataSetSize(std::size_t sizeDataSet);

      /** Safe function to add a data point. For more efficiency, one
       * can directly modify the matrix A and the vector b.
       */
      void addMeasure(jblas::vec const& A_row, double b_val);
			
      void solve();
			///@return the number of measures in the system
			int countMeasures() const { return m_countValues; }
			/**
			 * Merge the values of the \ref VariableSizeLinearLeastSquares
			 * given in argument.
			 * Only two \ref VariableSizeLinearLeastSquares of the same
			 * model size can be merged.
			 */
			void merge( const VariableSizeLinearLeastSquares2& );
    }; // class VariableLinearLeastSquares2

  } // namespace jmath
} // namespace jafar


#endif // HAVE_LAPACK
#endif // HAVE_BOOST_SANDBOX

#endif // JMATH_LINEAR_LEAST_SQUARES_HPP
