#include <Solver.hpp>
#include <algorithm>

namespace sat {

// =============================================================================
// Solver
// =============================================================================
Solver::Solver(int N, double a, int seed)
    : initialSeed(seed),
      randomBoolUD(0, 1),
      randomReal01UD(0, 1),
      N(N),
      alpha(a),
      wsMaxFlips(100 * N) {
  // Random number generator initialization
  if (seed = 0) initialSeed = rd();
  randomGenerator.seed(initialSeed);
}

// =============================================================================
// Algorithms
// =============================================================================
AlgorithmResult Solver::SID(FactorGraph* graph, double fraction) {
  fg = graph;
  sidFraction = fraction;

  // --------------------------------
  // Random initialization of surveys
  // --------------------------------
  for (Edge* edge : fg->edges) {
    edge->survey = getRandomReal01();
  }

  // Run until sat, sp unconverge or wlaksat result
  while (true) {
    // ----------------------------
    // Run SP
    // If trivial state is reach, walksat is called and the result returned
    // ----------------------------
    AlgorithmResult spResult = surveyPropagation();
    if (spResult != CONVERGE) return spResult;

    // --------------------------------
    // Build variable list and order it
    // --------------------------------
    vector<Variable*> unassignedVariables;

    // Evaluate and store the sum of the max bias of all unassigned variables
    double sumMaxBias = 0.0;
    for (Variable* var : fg->variables) {
      if (!var->assigned) {
        evaluateVar(var);
        sumMaxBias += var->Hp > var->Hm ? var->Hp : var->Hm;
      }
    }

    // Check paramagnetic state
    // TODO: Entender que significa esto, en el codigo original, este es
    // el unico sitio donde se llama a walksat
    if (sumMaxBias / unassignedVariables.size() < paramagneticState) {
      // TODO: Return walksat
      return WALKSAT;
    }

    // Assign minimum 1 variable
    int assignFraction = (int)(unassignedVariables.size() * fraction);
    if (assignFraction < 1) assignFraction = 1;
    sort(unassignedVariables.begin(), unassignedVariables.end(),
         [](const Variable* lvar, const Variable* rvar) {
           return abs(lvar->evalValue) > abs(rvar->evalValue);
         });

    // ------------------------
    // Fix the set of variables
    // ------------------------
    for (int i = 0; i < assignFraction; i++) {
      // Variables in the list can be already assigned due to UP being executed
      // in previous iterations
      if (unassignedVariables[i]->assigned) {
        i--;  // Don't count this variable as a new assignation
        continue;
      }

      // Found the new value and assign the variable
      // The assignation method cleans the graph and execute UP if one of
      // the cleaned clause become unitary
      Variable* var = unassignedVariables[i];

      // Recalculate biases for same reason, previous assignations clean the
      // graph and change relations
      evaluateVar(var);
      bool newValue = var->Hp > var->Hm ? true : false;

      if (!assignVariable(var, newValue)) {
        // Error found when assigning variable
        return CONTRADICTION;
      }
    }

    // Print graph status
    cout << fg << endl;

    // ----------------------------
    // If SAT finish algorithm
    // ----------------------------
    if (fg->IsSAT()) {
      return SAT;
    }
  }
}

AlgorithmResult Solver::surveyPropagation() {
  double maxConvergeDiff = 0.0;

  // Calculate subproducts of all variables
  computeSubProducts();

  for (int i = 0; i < spMaxIt; i++) {
    // Randomize clause iteration
    vector<Clause*> enabledClauses = fg->GetEnabledClauses();
    shuffle(enabledClauses.begin(), enabledClauses.end(), randomGenerator);

    // Calculate surveys
    for (Clause* clause : enabledClauses) {
      double maxConvDiffInClause = updateSurveys(clause);

      // Save max convergence diff
      if (maxConvDiffInClause > maxConvergeDiff)
        maxConvergeDiff = maxConvDiffInClause;
    }

    // Check if converged
    if (maxConvergeDiff <= spEpsilon) {
      // If max difference of convergence is 0, all are 0
      // which is a trivial state and walksat must be called
      if (maxConvergeDiff < ZERO_EPSILON) {
        // TODO: return walksat
        return WALKSAT;
      }

      // If not triavial return and continue algorith
      return CONVERGE;
    }
  }

  // Max itertions reach without convergence
  return UNCONVERGE;
}

void Solver::computeSubProducts() {
  for (Variable* var : fg->variables) {
    if (!var->assigned) {
      var->p = 1.0;
      var->m = 1.0;
      var->pzero = 0;
      var->mzero = 0;

      // For each edge connecting the variable to a clause
      for (Edge* edge : var->allNeighbourEdges) {
        if (edge->enabled) {
          // If edge is positive update positive subproduct of variable
          if (edge->type) {
            // If edge survey != 1
            if (1.0 - edge->survey > ZERO_EPSILON) {
              var->p *= 1.0 - edge->survey;
            }
            // If edge survey == 1
            else
              var->pzero++;
          }
          // If edge is negative, update negative subproduct of variable
          else {
            // If edge survey != 1
            if (1.0 - edge->survey > ZERO_EPSILON) {
              var->m *= 1.0 - edge->survey;
            }
            // If edge survey == 1
            else
              var->mzero++;
          }
        }
      }
    }
  }
}

double updateSurveys(Clause* clause) {
  double maxConvDiffInClause = 0.0;
  int zeros = 0;
  double allSubSurveys = 1.0;
  vector<double> subSurveys;

  // ==================================================================
  // Calculate subProducts of all literals and keep track of wich are 0
  // ==================================================================
  for (Edge* edge : clause->allNeighbourEdges) {
    if (edge->enabled) {
      Variable* var = edge->variable;
      double m, p, wn, wt;

      // If edge is positive:
      if (edge->type) {
        m = var->mzero ? 0 : var->m;
        if (var->pzero == 0)
          p = var->p / (1.0 - edge->survey);
        else if (var->pzero == 1 && (1.0 - edge->survey) < ZERO_EPSILON)
          p = var->p;
        else
          p = 0.0;

        wn = p * (1.0 - m);
        wt = m;
      }
      // If edge is negative
      else {
        p = var->pzero ? 0 : var->p;
        if (var->mzero == 0)
          m = var->m / (1.0 - edge->survey);
        else if (var->mzero == 1 && (1.0 - edge->survey) < ZERO_EPSILON)
          m = var->m;
        else
          m = 0.0;

        wn = m * (1 - p);
        wt = m;
      }

      // Calculate subSurvey
      double subSurvey = wn / (wn + wt);
      subSurveys.push_back(subSurvey);

      // If subsurvey is 0 keep track but don't multiply
      if (subSurvey < ZERO_EPSILON)
        zeros++;
      else
        allSubSurveys *= subSurvey;
    }
  }

  // =========================================================
  // Calculate the survey for each edge with the previous data
  // =========================================================
  int i = 0;
  for (Edge* edge : clause->allNeighbourEdges) {
    if (edge->enabled) {
      // ---------------------------------------------
      // Calculate new survey from sub survey products
      // ---------------------------------------------
      double newSurvey;
      // If there where no subSurveys == 0, proceed normaly
      if (!zeros) newSurvey = allSubSurveys / subSurveys[i];
      // If this subsurvey is the only one that is 0
      // consider the new survey as the total subSurveys
      else if (zeros == 1 && subSurveys[i] < ZERO_EPSILON)
        newSurvey = allSubSurveys;
      // If there where more that one subSurveys == 0, the new survey is 0
      else
        newSurvey = 0.0;

      // ----------------------------------------------------
      // Update the variable subproducts with new survey info
      // ----------------------------------------------------
      Variable* var = edge->variable;
      // If edge is positive update positive subproduct
      if (edge->type) {
        // If previous survey != 1 (with an epsilon margin)
        if (1.0 - edge->survey > ZERO_EPSILON) {
          // If new survey != 1, update the sub product with the difference
          if (1.0 - newSurvey > ZERO_EPSILON)
            var->p *= ((1.0 - newSurvey) / (1.0 - edge->survey));
          // If new survey == 1, update the subproduct by remove the old survey
          // and keep track of the new survey == 1 (pzero++)
          else {
            var->p /= (1.0 - edge->survey);
            var->pzero++;
          }
        }
        // If previous survey == 1
        else {
          // If new survey == 1, don't do anything (both surveys are the same)
          // If new survey != 1, update subproduct
          if (1.0 - newSurvey > ZERO_EPSILON) {
            var->p *= (1.0 - newSurvey);
            var->pzero--;
          }
        }
      }
      // If edge is negative, update negative subproduct
      else {
        // If previous survey != 1 (with an epsilon margin)
        if (1.0 - edge->survey > ZERO_EPSILON) {
          // If new survey != 1, update the sub product with the difference
          if (1.0 - newSurvey > ZERO_EPSILON)
            var->m *= ((1.0 - newSurvey) / (1.0 - edge->survey));
          // If new survey == 1, update the subproduct by remove the old survey
          // and keep track of the new survey == 1 (pzero++)
          else {
            var->m /= (1.0 - edge->survey);
            var->mzero++;
          }
        }
        // If previous survey == 1
        else {
          // If new survey == 1, don't do anything (both surveys are the same)
          // If new survey != 1, update subproduct
          if (1.0 - newSurvey > ZERO_EPSILON) {
            var->m *= (1.0 - newSurvey);
            var->mzero--;
          }
        }
      }

      // ----------------------------------------------------
      // Store new survey and update max clause converge diff
      // ----------------------------------------------------
      double edgeConvDiff = abs(edge->survey - newSurvey);
      if (edgeConvDiff > maxConvDiffInClause)
        maxConvDiffInClause = edgeConvDiff;

      edge->survey = newSurvey;
      i++;
    }
  }

  return maxConvDiffInClause;
}

bool Solver::assignVariable(Variable* var, bool value) {
  // Contradiction if variable was already assigned with different value
  if (var->assigned && var->value != value) {
    cout << "ERROR: Variable X" << var->id
         << " already assigned with opposite value" << endl;
    return false;
  }

  var->AssignValue(value);
  return cleanGraph(var);
}

bool Solver::cleanGraph(Variable* var) {
  for (Edge* edge : var->allNeighbourEdges) {
    if (edge->enabled) {
      if (edge->type == var->value) {
        edge->clause->Dissable();
      } else {
        edge->Dissable();

        // Execute UP for this clause because can become unitary or empty
        if (!unitPropagation(edge->clause)) return false;
      }
    }
  }

  return true;
}

bool Solver::unitPropagation(Clause* clause) {
  vector<Edge*> enabledEdges = clause->GetEnabledEdges();
  int size = enabledEdges.size();

  // Contradiction if empty clause
  if (size == 0) {
    cout << "ERROR: Clause C" << clause->id << " is empty" << endl;
    return false;
  }

  // Unitary clause
  if (size == 1) {
    // Fix the variable to the edge type. This will execute UP with recursivity
    Edge* edge = enabledEdges[0];  // Unique enabled edge in unitary clause
    return assignVariable(edge->variable, edge->type);
  }

  // Finish unit propagation if clause is not unitary
  return true;
}

void Solver::evaluateVar(Variable* var) {
  double p = var->pzero ? 0 : var->p;
  double m = var->mzero ? 0 : var->m;

  var->Hz = p * m;
  var->Hp = m - var->Hz;
  var->Hm = p - var->Hz;

  // Normalize
  double sum = var->Hm + var->Hz + var->Hp;
  var->Hz /= sum;
  var->Hp /= sum;
  var->Hm /= sum;

  // Store eval value
  var->evalValue = abs(var->Hp - var->Hm);
}

}  // namespace sat