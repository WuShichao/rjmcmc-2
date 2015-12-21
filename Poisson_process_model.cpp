#include "Poisson_process_model.hpp"
#include <gsl/gsl_sf_gamma.h>
#include <math.h>
#include "decay_function.hpp"

pp_model::pp_model(double alpha,double beta, Data<double> * data, Step_Function* time_scale, Step_Function* seasonal_scale )
:probability_model(data,seasonal_scale),m_alpha(alpha),m_beta(beta)
{
   m_pp_time_scale = (Univariate_Function*)(time_scale ? time_scale : m_seasonal_scale);
   m_cum_counts = NULL;
   construct();
}

pp_model::pp_model(vector<string>* data_filenames, double alpha, double beta, double start, double end, double season )
:probability_model(data_filenames,start,end,season),m_alpha(alpha),m_beta(beta)
{
   m_cum_counts = NULL;
   m_pp_time_scale = NULL;
   construct();
}

pp_model::pp_model(vector<string>* data_filenames)
:probability_model(data_filenames)
{
   m_cum_counts = NULL;
   m_pp_time_scale = NULL;
   construct_empirical_prior();
}

pp_model::pp_model(double alpha,double beta, double rate, Data<double> * data)
:probability_model(data),m_alpha(alpha),m_beta(beta)
{
  construct();
  m_shot_noise_rate = rate;
  if(m_shot_noise_rate>0)
    m_pp_time_scale = (Univariate_Function*)new Decay_Function(m_shot_noise_rate);
}

pp_model::pp_model(string data_filename,double alpha, double beta):
probability_model(),m_alpha(alpha),m_beta(beta)
{
  m_cum_counts = new Data<unsigned long long int>(data_filename.c_str(),false);
  m_owner_of_data = true;
  poisson_regression_construct();
}

pp_model::pp_model(Data<unsigned long long int>* count_data, Data<double>* time_data, double alpha, double beta, double* intensity_multipliers):
probability_model(time_data),m_cum_counts(count_data),m_alpha(alpha),m_beta(beta)
{
  poisson_regression_construct();
  if(intensity_multipliers){
    unsigned long long int data_length = count_data->get_cols();
    m_cum_intensity_multipliers = new double[data_length];
    m_cum_intensity_multipliers[0] = intensity_multipliers[0];
    for(unsigned long long int i = 0; i < data_length; i++)
      m_cum_intensity_multipliers[i]=m_cum_intensity_multipliers[i-1]+intensity_multipliers[i];
  }
}


pp_model::~pp_model(){
  if(m_poisson_regression && m_cum_counts )
  {
    if(m_owner_of_data)
      delete m_cum_counts;
    else
      m_cum_counts->undo_replace_with_cumulative();
  }
  if(m_cum_intensity_multipliers)
    delete [] m_cum_intensity_multipliers;

  if(m_pp_time_scale && m_shot_noise_rate > 0) 
    delete (Decay_Function*)m_pp_time_scale;
}

void pp_model::use_random_mean(int seed) {
  m_random_mean = 1;
  m_rng = gsl_rng_alloc(gsl_rng_taus);
  gsl_rng_set(m_rng, seed);
}

void pp_model::construct(){
  m_likelihood_term_zero = m_alpha*log(m_beta);
  m_likelihood_term = m_likelihood_term_zero-gsl_sf_lngamma(m_alpha);
  if( m_seasonal_analysis )
    collapse_to_seasons();
  m_poisson_regression = false;
  m_cum_intensity_multipliers = NULL;
  m_shot_noise_rate = 0.0;
  m_random_mean = 0;
}

void pp_model::poisson_regression_construct(){
  construct();
  m_poisson_regression = true;
  //  double log_sum_fact_counts = 0;
  //  for(unsigned long long int i = 0; i < m_cum_counts->get_cols(); i++)
  //    log_sum_fact_counts += gsl_sf_lnfact((*m_cum_counts)[0][i]);
  m_cum_counts->replace_with_cumulative();
}

void pp_model::construct_empirical_prior(){
  double factor = .1;
  double t = factor * m_data_cont->get_element(0,m_data_cont->get_cols()-1);
  m_beta = (m_pp_time_scale ? m_pp_time_scale->cumulative_function(t) : t);
  unsigned long long int r = m_data_cont ? m_data_cont->find_data_index(t) : 0;
  m_alpha = r > 0 ? r : 1;
  construct();
}

double pp_model::log_likelihood_interval(changepoint *obj1, changepoint *obj2){
  unsigned long long int i1 = obj1->getdataindex();
  unsigned long long int i2 = obj2->getdataindex();
  if(m_poisson_regression)
    return poisson_regression_log_likelihood_interval(i1,i2);
  m_r = i2 - i1;//number of uncensored observations in each interval
  double t1 = obj1->getchangepoint();
  double t2 = obj2->getchangepoint();
  return log_likelihood_interval_with_count(t1,t2,m_r);
}

double pp_model::log_likelihood_up_to(double t){
  if(m_poisson_regression)
    return poisson_regression_log_likelihood_interval(0,static_cast<int>(ceil(t)));
  m_r = m_data_cont ? m_data_cont->find_data_index(t) : 0;
  return log_likelihood_interval_with_count(0,t,m_r);
}

double pp_model::log_likelihood_interval(double t1, double t2){
  if(m_data_cont){
    unsigned long long int r1 = m_data_cont->find_data_index(t1);
    unsigned long long int r2 = m_data_cont->find_data_index(t2,0,r1);//assumes t2>=t1
    if(m_poisson_regression)
      return poisson_regression_log_likelihood_interval(r1,r2);
    m_r = r2-r1;
  }else{
    if(m_poisson_regression)
      return poisson_regression_log_likelihood_interval(static_cast<int>(ceil(t1)),static_cast<int>(ceil(t2)));
    m_r = 0;
  }
  return log_likelihood_interval_with_count(t1,t2,m_r);
}

double pp_model::log_likelihood_interval_with_count(double t1, double t2, unsigned long long int r){
  m_t = m_pp_time_scale ? m_pp_time_scale->cumulative_function( t1, t2 ) : t2-t1;//length of interval
  if(m_t<0){
    cerr<<m_t<<" "<<t2<<" "<<t1<<" "<<m_pp_time_scale->cumulative_function(t1,t2)<<" Poisson_process_model.h: length of time interval cannot be less than 0"<<endl;
    return -1e300;
  }
  return (m_shot_noise_rate*r*t1)+log_likelihood_length_and_count(m_t,r);
}

double pp_model::log_likelihood_length_and_count(double t, unsigned long long int r){
    if(t<=0)
      return 0;
    if(!r)
      return m_likelihood_term_zero - m_alpha*log(m_beta+t);
    return m_likelihood_term + gsl_sf_lngamma(r+m_alpha) - (r+m_alpha)*log(m_beta+t);
}

double pp_model::poisson_regression_log_likelihood_interval(unsigned long long int i1, unsigned long long int i2){
  if(i1==i2)
    return 0;
  if(!m_cum_intensity_multipliers)
    m_t = i2 - i1;
  else
    m_t = (i2>0 ? m_cum_intensity_multipliers[i2-1] : 0) - (i1>0 ? m_cum_intensity_multipliers[i1-1] : 0);
  m_r = (i2>0 ? (*m_cum_counts)[0][i2-1] : 0) - (i1>0 ? (*m_cum_counts)[0][i1-1] : 0);
  return log_likelihood_length_and_count(m_t,m_r);
}

void pp_model::calculate_posterior_mean_parameters(changepoint *obj1, changepoint *obj2){
  //number of uncensored observations in each interval
  unsigned long long int i1 = obj1->getdataindex();
  unsigned long long int i2 = obj2->getdataindex();
  if (i2<i1){
    cerr<<"Poisson_process_model.h: number of datapoints can not be less than 0"<<endl;
    exit(1);
  }

  double d, r;

  if(m_poisson_regression){
    d = i2-i1;
    r = (i2>0 ? (*m_cum_counts)[0][i2-1] : 0) - (i1>0 ? (*m_cum_counts)[0][i1-1] : 0);
  }else{
    r = i2-i1;
    //length of interval
    double t1 = obj1->getchangepoint();
    double t2 = obj2->getchangepoint();
    d = m_pp_time_scale ? m_pp_time_scale->cumulative_function( t1, t2 ) : t2-t1;
    if(d<0){
      cerr<<"Poisson_process_model.h: changepoints are not ordered"<<" "<<t1<< " "<<t2<<" "<<m_pp_time_scale->cumulative_function(t1,t2)<<endl;
      exit(1);
    }
  }
  m_alpha_star=m_alpha+r;
  m_beta_star=m_beta+d;
}

double pp_model::calculate_mean(changepoint *obj1, changepoint *obj2){
  if (!m_random_mean) {
    calculate_posterior_mean_parameters(obj1,obj2);
    m_mean = m_alpha_star/m_beta_star;
  } else {
    m_mean = draw_mean_from_posterior(obj1, obj2);
  }
  m_var = m_mean/m_beta_star;
  return m_mean;
}

double pp_model::draw_mean_from_posterior(changepoint *obj1, changepoint *obj2){
   calculate_posterior_mean_parameters(obj1,obj2);
   if(!m_rng){
      m_rng = gsl_rng_alloc(gsl_rng_taus);
      gsl_rng_set (m_rng,0);
   }
   m_mean=gsl_ran_gamma(m_rng,m_alpha_star,1.0/m_beta_star);
   return m_mean;
}

double pp_model::calculate_log_predictive_df(double t1, double t2, double t3, bool lower_tail ){
  double t = m_pp_time_scale ? m_pp_time_scale->cumulative_function( t2, t3 ) : t3-t2;
  if(t<=0)
    return 0;
  unsigned long long int r2 = m_data_cont ? m_data_cont->find_data_index(t2) : 0;
  unsigned long long int r = m_data_cont ? m_data_cont->find_data_index(t3,0,r2) - r2 : 0;//assumes t3>=t2
  if(!lower_tail && !r)
    return 0;
  m_r = r2 - (m_data_cont ? m_data_cont->find_data_index(t1,0,0,r2) : 0);//assumes t2>=t1
  m_t = m_pp_time_scale ? m_pp_time_scale->cumulative_function( t1, t2 ) : t2-t1;
  m_alpha_star = m_alpha + m_r;
  m_beta_star = m_beta + m_t;
  return calculate_log_posterior_predictive_df(t,r,lower_tail);
}

double pp_model::calculate_log_posterior_predictive_pdf( double t, unsigned long long int r ){
  if(t<=0)
    return r == 0 ? 0 : -DBL_MAX;
  m_log_pdf_const = m_alpha_star*(log(m_beta_star)-log(m_beta_star+t));
  double e_log_pdf_const = exp(m_log_pdf_const);
  double t_b = t/(m_beta_star+t);
  double log_t_b = log(t)-log(m_beta_star+t);
  m_log_predictive_pdf = m_log_pdf_const;
  double pmf_sum = 1, pmf = 1;
  m_minimum_tail = e_log_pdf_const*pmf_sum;
  for( unsigned long long int i=0; i<r; i++ ){
    pmf *= t_b * (m_alpha_star+i)/(i+1);
    m_log_predictive_pdf += log_t_b + log(m_alpha_star+i) - log(i+1);
    if(i==r-1)
      m_minimum_tail = 1 - e_log_pdf_const*pmf_sum;
    pmf_sum += pmf;
    if((i==r-1)&&m_minimum_tail>e_log_pdf_const*pmf_sum)
      m_minimum_tail = e_log_pdf_const*pmf_sum;
  }
  return m_log_predictive_pdf;
}

double pp_model::calculate_log_posterior_predictive_df( double t, unsigned long long int r, bool lower_tail ){
  if(t<=0)// || (!lower_tail && !r ))
    return -LOG_TWO;
  double t_b = t/(m_beta_star+t);
  double log_t_b = log(t)-log(m_beta_star+t);
  m_log_pdf_const = m_alpha_star*(log(m_beta_star)-log(m_beta_star+t));
  double e_log_pdf_const = exp(m_log_pdf_const);
  double pmf_sum = 1, pmf = 1;
  double log_pmf = 0;
  double df = 0, df2 = 0;
  unsigned long long int i=0;
  bool found_mid_point = false;
  m_survivor_midpoint = 1;
  bool keep_looping = true;
  //  m_log_predictive_pdf = DBL_MAX;
  double g_i = e_log_pdf_const*pmf_sum;
  m_predictive_two_sided_df = m_predictive_two_sided_df2 = 0;
  m_df_values.clear();
  m_pdf_values.clear();
  while( keep_looping ){
    if(i>0){
      log_pmf += log_t_b + log(m_alpha_star+i-1) - log(i);
      pmf *= t_b * (m_alpha_star+i-1)/i;
      //      if(!found_mid_point){
	////        if(exp(m_log_pdf_const)*pmf_sum >= (1-exp(m_log_pdf_const)*pmf)/2){
      //        if(pmf_sum >= (e_minus_log_pdf_const-pmf)/2){
      //          m_survivor_midpoint = 1-pmf_sum/e_minus_log_pdf_const;
      //          found_mid_point = true;
      //        }
      //      }
      g_i = 1 - e_log_pdf_const*pmf_sum;
      pmf_sum += pmf;
      if(!found_mid_point && g_i>e_log_pdf_const*pmf_sum)
	g_i = e_log_pdf_const*pmf_sum;
    }
    if(!found_mid_point){
      if(m_log_pdf_const + log(pmf_sum ) >= -LOG_TWO ){
	found_mid_point = true;
	m_survivor_midpoint = g_i;
      }
    }
    //    m_df_values.push_back(g_i);
    //    m_pdf_values.push_back(pmf*e_log_pdf_const);
    if(pmf<=0||m_predictive_two_sided_df2>=1){//rounding errors
      m_predictive_two_sided_df = m_minimum_tail + e_log_pdf_const;
      m_predictive_two_sided_df2 = m_minimum_tail;
      keep_looping = false;
    }else{
      if(i!=r&&g_i>m_minimum_tail)
	m_predictive_two_sided_df += pmf*e_log_pdf_const;
      if(i==r||g_i>=m_minimum_tail)
	m_predictive_two_sided_df2 += pmf*e_log_pdf_const;
    }
    if(i==r-1){
      if(lower_tail)
	df2 = m_log_pdf_const + log(pmf_sum);
      else{
//	df = log(1 - exp(m_log_pdf_const)*pmf_sum);
	df = m_log_pdf_const + log(exp(-m_log_pdf_const)-pmf_sum);
        if(df>=0)
          df = -exp(m_log_pdf_const)*pmf_sum;
      }
    }else if(i==r){
      m_log_predictive_pdf = log_pmf + m_log_pdf_const;
      m_log_predictive_df = g_i;
      if(lower_tail)
	df = m_log_pdf_const + log(pmf_sum);
      else{
//	df2 = log(1 - exp(m_log_pdf_const)*pmf_sum);
	df2 = m_log_pdf_const + log(exp(-m_log_pdf_const)-pmf_sum);
	if(log_pmf + m_log_pdf_const>df)//due to rounding errors
	  df = log_pmf + m_log_pdf_const;
        if(df2>=0)
          df2 = -exp(m_log_pdf_const)*pmf_sum;
      }
    }
    i++;
    if(keep_looping){
      keep_looping = i<=r || !found_mid_point || g_i >= m_log_predictive_df;
      if(!keep_looping){
	m_predictive_two_sided_df = 1-m_predictive_two_sided_df;
	m_predictive_two_sided_df2 = 1-m_predictive_two_sided_df2;
      }
    }
  }
//  double edf = exp(df), edf2 = exp(df2);
////  double edfdiff = exp(df2-df);
  ////  if(lower_tail&&!r)
  ////    edfdiff=0;//df2 should be -infinity
//    edf2=0;//df2 should be -infinity
  //return (edf*df-edf2*df2)/(edf-edf2) - 1;//mean of log(p) when p~U(edf,edf2)
//  cout << edf*.5+edf2*.5 << endl;
//  cout << df << "  " << df2<< "  " << log_pmf << "  " << m_log_pdf_const<< endl;
//  return log(edf*.5+edf2*.5);//median of log(p) when p~U(edf,edf2)
////  return df+(log(1+edfdiff)-LOG_TWO);//median of log(p) when p~U(edf,edf2)
  //  return df;
  m_pvalue_pair = make_pair(exp(df2),exp(df));
  //  m_pvalue_pair = make_pair(df2,df);
  m_p_value_endpoints.clear();
  m_p_value_endpoints.push_back(m_pvalue_pair);
  m_pvalue_pair_on_log_scale = false;
  m_p_value_endpoints_log_scale.clear();
  m_p_value_endpoints_log_scale.push_back(m_pvalue_pair_on_log_scale);
  return combine_p_values_from_endpoints(false);
}

void pp_model::calculate_sequential_log_predictive_dfs(double start, double end, double increment, bool lower_tail, bool two_sided, double control_chart_weight, string* filename_ptr, vector<double>* dfs ){
  probability_model::set_parameters_to_current_t(start);
  ofstream outfile;
  if( filename_ptr )
    outfile.open(filename_ptr->c_str());
  while( m_current_t < end ){
    calculate_log_predictive_df_bounds(increment,lower_tail,two_sided);
    if(dfs)
      dfs->push_back(m_log_predictive_df);
    if(outfile.is_open())
      outfile << exp(m_log_predictive_df) << endl;
    m_current_t += increment;
  }
}

void pp_model::set_parameters_to_current_t(){
  m_current_data_index = m_r = m_data_cont ? m_data_cont->find_data_index(m_current_t) : 0;
  m_t = m_pp_time_scale ? m_pp_time_scale->cumulative_function( 0, m_current_t ) : m_current_t;
  m_alpha_star = m_alpha + m_r;
  m_beta_star = m_beta + m_t;
}

double pp_model::calculate_log_predictive_df_bounds( double increment, bool lower_tail, bool two_sided, bool increment_parameters ){
  if(!m_p_value_alternative_style)
    return calculate_event_count_log_predictive_df(increment,lower_tail,two_sided,increment_parameters);
  return calculate_waiting_times_log_predictive_df(increment,lower_tail,two_sided,increment_parameters);
}

double pp_model::calculate_event_count_log_predictive_df( double increment, bool lower_tail, bool two_sided, bool increment_parameters ){
  double t = m_pp_time_scale ? m_pp_time_scale->cumulative_function( m_current_t, m_current_t+increment ) : increment;
  unsigned long long int r = m_data_cont ? m_data_cont->find_data_index(m_current_t+increment,0,m_r) - m_r : 0;
  if(t>0){
    if(two_sided)
      calculate_log_posterior_predictive_pdf(t,r);
    m_log_predictive_df = calculate_log_posterior_predictive_df(t,r,lower_tail);
    if(two_sided){
      //      m_predictive_two_sided_df = 1-m_predictive_two_sided_df*exp(m_log_pdf_const);
      //      m_predictive_two_sided_df2 = 1-m_predictive_two_sided_df2*exp(m_log_pdf_const);
      ////      m_log_predictive_df = log(m_predictive_two_sided_df+m_predictive_two_sided_df2) - LOG_TWO;
      //      m_log_predictive_df = get_two_sided_discrete_p_value(r);
      //      m_pvalue_pair = make_pair(log(m_predictive_two_sided_df2),log(m_predictive_two_sided_df));
      m_pvalue_pair = make_pair(m_predictive_two_sided_df2,m_predictive_two_sided_df);
      m_p_value_endpoints.clear();
      m_p_value_endpoints.push_back(m_pvalue_pair);
      m_pvalue_pair_on_log_scale = false;
      m_p_value_endpoints_log_scale.clear();
      m_p_value_endpoints_log_scale.push_back(m_pvalue_pair_on_log_scale);
      m_log_predictive_df = combine_p_values_from_endpoints(false);
    }
    m_currently_observable = true;
  }else{
    m_log_predictive_df = -LOG_TWO;
    m_currently_observable = false;
  }
  m_r += r;
  m_t += t;
  m_alpha_star += r;
  m_beta_star += t;
  m_current_data_index = m_r;
  return m_log_predictive_df;
}

double pp_model::calculate_waiting_times_log_predictive_df( double increment, bool lower_tail, bool two_sided, bool increment_parameters ){
  double sum_log_pvals = 0;// sum_log_pvals2 = 0;
  unsigned long long int how_many = 0;
  unsigned long long int i2 = m_data_cont ? m_data_cont->find_data_index(m_current_t+increment,0,m_current_data_index) : (unsigned long long int)(m_current_t+increment);
  double current_t = m_current_t;
  if(i2>m_current_data_index){
    while(i2>m_current_data_index){
      double t = m_pp_time_scale ? m_pp_time_scale->cumulative_function( current_t, (*m_data_cont)[0][m_current_data_index] ) : (*m_data_cont)[0][m_current_data_index] - current_t;
      m_log_predictive_df = calculate_log_posterior_predictive_pdf(t,0);//upper tail
      if(t<=0){
	cerr << "Rounding errors in event times"<< endl;
	exit(1);
      }
      if(lower_tail||two_sided){
	m_log_predictive_df2 = log(1-exp(m_log_predictive_df));//lower tail
	if(lower_tail)
	  m_log_predictive_df = m_log_predictive_df2;
      }
      /*      if(two_sided){//two-sided p-values version two - combine at the start
	if(m_log_predictive_df > m_log_predictive_df2)
	  m_log_predictive_df = m_log_predictive_df2;
	  m_log_predictive_df += LOG_TWO;
      }*/
      sum_log_pvals += m_log_predictive_df;
      //      if(two_sided)//two-sided p-values version one - combine at the end
      //	sum_log_pvals2 += m_log_predictive_df2;
      m_r++;
      m_t += t;
      m_alpha_star++;
      m_beta_star += t;
      how_many++;
      current_t = (*m_data_cont)[0][m_current_data_index];
      m_current_data_index++;
    }
  }
  double t = (m_pp_time_scale&&m_data_cont) ? m_pp_time_scale->cumulative_function( current_t, m_current_t+increment ) : m_current_t+increment - current_t;
  if(t>0){
    m_log_predictive_df = calculate_log_posterior_predictive_pdf(t,0) - LOG_TWO;//upper tail
    if(lower_tail||two_sided){
      m_log_predictive_df2 = log(1-exp(m_log_predictive_df));//lower tail
      if(lower_tail)
	m_log_predictive_df = m_log_predictive_df2;
    }
    /*    if(two_sided){
      if(m_log_predictive_df > m_log_predictive_df2)
	m_log_predictive_df = m_log_predictive_df2;
      m_log_predictive_df += LOG_TWO;
      }*/
    sum_log_pvals += m_log_predictive_df;
    //    if(two_sided)
    //      sum_log_pvals2 += m_log_predictive_df2;
    m_t += t;
    m_beta_star += t;
    how_many++;
  }
  //  if(two_sided && sum_log_pvals2 < sum_log_pvals)
  //    sum_log_pvals = sum_log_pvals2;
  m_currently_observable = how_many > 0;
  if(m_currently_observable){
    m_log_predictive_df = how_many==1 ? sum_log_pvals : log(gsl_cdf_chisq_Q(-2*sum_log_pvals, 2*how_many));
    //    if(two_sided)
    //      m_log_predictive_df += LOG_TWO;
    if(two_sided){
    if(m_log_predictive_df>=0)//rounding erros
      m_log_predictive_df2 = log(gsl_cdf_chisq_P(-2*sum_log_pvals, 2*how_many));
    else
      m_log_predictive_df2 = log(1-exp(m_log_predictive_df));//lower tail
      if(m_log_predictive_df > m_log_predictive_df2)
	m_log_predictive_df = m_log_predictive_df2;
      m_log_predictive_df += LOG_TWO;
    }
  }
  else
    m_log_predictive_df = -LOG_TWO;
  m_pvalue_pair = make_pair(m_log_predictive_df,m_log_predictive_df);
  m_pvalue_pair_on_log_scale = true;
  return m_log_predictive_df;
}

double pp_model::log_likelihood_changepoints( vector<unsigned long long int>& regime_changepoints_data_indices, vector<double>& regime_changepoints_changepoint_positions ){
  m_r = 0;
  m_t = 0;
  for( unsigned int i = 0; i < regime_changepoints_data_indices.size(); i += 2 ){
    //number of uncensored observations in each interval
    m_r += regime_changepoints_data_indices[ i + 1 ] - regime_changepoints_data_indices[ i ];
    //Calculate the time interval
    double t1 = regime_changepoints_changepoint_positions[ i ];
    double t2 = regime_changepoints_changepoint_positions[ i + 1 ];
    m_t += m_time_scale ? m_time_scale->cumulative_function( t1, t2 ) : t2-t1;
  }
  return log_likelihood_length_and_count( m_t, m_r );
}
