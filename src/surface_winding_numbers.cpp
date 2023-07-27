#include "surface_winding_numbers.h"

// ==== SOLVE

/*
 * Input: A discrete primal 1-chain ∈ Z^|E| encoding the input curve Γ.
 *
 * Output: The winding number function on corners. Corners adjacent to interior endpoints will have placeholder values
 * SPECIAL_VAL; their values are to be interpolated using the scheme described in Section 2.3.2.
 */
CornerData<double> SurfaceWindingNumbersSolver::solve(const Vector<double>& chain) const {

    // Pre-compute curve quantities.
    VertexData<bool> isInteriorEndpoint(mesh, false);
    VertexData<bool> isInteriorVertex(mesh, false); // debugging
    std::vector<Vertex> interiorVertices;
    // Warning: On non-orientable meshes, applying the boundary operator (computed as a simple adjacency matrix) may not
    // yield the expected results, e.g. there may be non-zero boundary for what should be closed curve.
    Vector<double> boundary = d0T * chain;
    double eps = 1e-5;
    bool isCurveClosed = true;
    // Check if the curve is closed relative to the boundary.
    // In the same loop, pre-compute a few curve quantities needed downstream, namely,
    //  - determine the interior vertices of the curve
    //  - store an arbitrary outgoing cut halfedge per interior vertex
    //  - curve endpoints and their signs.
    std::map<Vertex, Halfedge> outgoingHalfedgeOnCurve;
    // TODO: If boundary vertex, outgoingHalfedge should be on the most CW corner
    // TODO: Did I handle chain edges on the boundary yet?
    std::vector<std::pair<Vertex, bool>> endpoints;
    geom.requireVertexIndices();
    geom.requireEdgeIndices();
    for (Vertex v : mesh.vertices()) {
        size_t vIdx = geom.vertexIndices[v];
        if (abs(boundary[vIdx]) > eps && !v.isBoundary() && v.isManifold()) {
            isCurveClosed = false;
            isInteriorEndpoint[v] = true;
            bool sgn = (boundary[vIdx] > 0);
            size_t mag = abs(boundary[vIdx]);
            for (size_t i = 0; i < mag; i++) endpoints.emplace_back(v, sgn);
        } else if (v.isManifold()) {
            for (Halfedge he : v.outgoingHalfedges()) {
                if (abs(chain[geom.edgeIndices[he.edge()]]) > eps) {
                    interiorVertices.push_back(v);
                    outgoingHalfedgeOnCurve.insert(std::make_pair(v, he));
                    isInteriorVertex[v] = true;
                    break;
                }
            }
        }
    }
    geom.unrequireVertexIndices();
    geom.unrequireEdgeIndices();

    CornerData<double> c = computeReducedCoordinates(chain, interiorVertices, outgoingHalfedgeOnCurve);
    polyscope::getSurfaceMesh("input mesh")->addVertexScalarQuantity("isInteriorVertex", isInteriorVertex); // debugging
    polyscope::getSurfaceMesh("input mesh")->addVertexScalarQuantity("isInteriorEndpoint", isInteriorEndpoint);
    CornerData<double> w = solveJumpEquation(interiorVertices, isInteriorEndpoint, c);
    polyscope::getSurfaceMesh("input mesh")->addCornerScalarQuantity("u", w); // debugging
    std::cerr << "u min: " << w.toVector().minCoeff() << "\tu max: " << w.toVector().maxCoeff()
              << std::endl; // debugging
    if (!simplyConnected && doHomologyCorrection) {
        std::cerr << "Doing homology correction..." << std::endl;
        Vector<double> gamma = DarbouxDerivative(isInteriorEndpoint, w);
        polyscope::getSurfaceMesh("input mesh")
            ->addOneFormIntrinsicVectorQuantity("omega", gamma, polyscopeEdgeOrientations(mesh)); // debugging
        if (!isCurveClosed) gamma = harmonicComponent(gamma);
        polyscope::getSurfaceMesh("input mesh")
            ->addOneFormIntrinsicVectorQuantity("gamma", gamma, polyscopeEdgeOrientations(mesh)); // debugging
        CornerData<double> v =
            approximateResidual ? approximateResidualFunction(chain, endpoints, gamma) : residualFunction(chain, gamma);
        polyscope::getSurfaceMesh("input mesh")->addCornerScalarQuantity("v", v); // debugging
        std::cerr << "v min: " << v.toVector().minCoeff() << "\tv max: " << v.toVector().maxCoeff()
                  << std::endl; // debugging
        c = subtractJumpDerivative(chain, interiorVertices, isInteriorEndpoint, outgoingHalfedgeOnCurve, v);
        polyscope::getSurfaceMesh("input mesh")->addCornerScalarQuantity("c", c); // debugging
        w = solveJumpEquation(interiorVertices, isInteriorEndpoint, c);
        // TODO: Do rounding procedure
    }
    // TODO: Store intermediate computed quantities as member variables.
    return w;
}

// input options: the chain directly (on orientable meshes, primal and dual 1-chain are in correspondence), sequence of
// vertices, collection of halfedges, pairs of faces (for non-manifold or non-orientable meshes)

CornerData<double> SurfaceWindingNumbersSolver::solve(const std::vector<Vertex>& curve) const {

    // Convert the input curve to a 1-chain, then call generic solver.
    Vector<double> chain = convertToChain(curve);
    return solve(chain);
}

CornerData<double> SurfaceWindingNumbersSolver::solve(const std::vector<std::vector<Vertex>>& curves) const {

    // Convert the input curve to a 1-chain, then call generic solver.
    Vector<double> chain = Vector<double>::Zero(mesh.nEdges());
    for (const auto& curve : curves) chain += convertToChain(curve);
    return solve(chain);
}

CornerData<double> SurfaceWindingNumbersSolver::solve(const std::vector<Halfedge>& curve) const {

    // Convert the input curve to a 1-chain, then call generic solver.
    Vector<double> chain = convertToChain(curve);
    return solve(chain);
}

CornerData<double> SurfaceWindingNumbersSolver::solve(const std::vector<std::array<Face, 2>>& curve) const {

    // TODO
    return CornerData<double>(mesh, 0);
}

CornerData<double> SurfaceWindingNumbersSolver::solve(const std::vector<SurfacePoint>& curveNodes,
                                                      const std::vector<std::array<size_t, 2>>& curveEdges) const {

    // TODO: Call Poisson solver
    return CornerData<double>(mesh, 0);
}


// ==== ALGORITHM STEPS


/*
 * Input: The curve Γ, represented by relevant pre-computed quantities.
 *
 * Output: A function c on corners, that expresses values at corners relative to a reference value at a corner adjacent
 * to the same vertex.
 */
CornerData<double> SurfaceWindingNumbersSolver::computeReducedCoordinates(
    const Vector<double>& chain, const std::vector<Vertex>& interiorVertices,
    const std::map<Vertex, Halfedge>& outgoingHalfedgeOnCurve) const {

    geom.requireEdgeIndices();
    CornerData<double> reducedCoordinates(mesh, 0);
    for (const auto& vi : interiorVertices) {
        // we should have already filtered for nonmanifold vertices when we computed interiorVertices
        Halfedge start = outgoingHalfedgeOnCurve.at(vi);
        Halfedge curr = start;
        double cumJump = 0.; // cumulative jump
        do {
            if (curr.isInterior()) {
                // always have a jump of 0 across edges on the boundary
                if (!curr.edge().isBoundary()) {
                    double jump = chain[geom.edgeIndices[curr.edge()]];
                    cumJump += (curr.orientation() ? jump : -jump);
                }
                reducedCoordinates[curr.corner()] = cumJump;
            }
            curr = curr.next().next().twin(); // go counterclockwise
        } while (curr != start);
    }
    geom.unrequireEdgeIndices();
    return reducedCoordinates;
}

/*
 * Input: The curve Γ, represented by relevant pre-computed quantities: its interior vertices, interior endpoints, and
 * reduced coordinates.
 *
 * Output: A function u. If doing projective interpolation (Section 2.3.2), values at corners adjacent to interior
 * endpoints are left undefined, to be interpolated later.
 */
CornerData<double> SurfaceWindingNumbersSolver::solveJumpEquation(const std::vector<Vertex>& interiorVertices,
                                                                  const VertexData<bool>& isInteriorEndpoint,
                                                                  const CornerData<double>& reducedCoordinates) const {

    // Omit curve endpoints from the system. First map existing vertices to their new DOF indices.
    VertexData<size_t> DOFindex(mesh);
    size_t nDOFs = 0;
    for (Vertex v : mesh.vertices()) {
        if (!isInteriorEndpoint[v]) {
            DOFindex[v] = nDOFs;
            nDOFs++;
        }
    }

    geom.requireHalfedgeCotanWeights();
    SparseMatrix<double> L = buildLaplacian(isInteriorEndpoint, DOFindex, nDOFs);
    Vector<double> b = buildJumpLaplaceRHS(interiorVertices, isInteriorEndpoint, reducedCoordinates, DOFindex, nDOFs);
    geom.unrequireHalfedgeCotanWeights();
    shiftDiagonal(L, 1e-8); // hack to ensure L is PD and not just PSD
    Vector<double> u0 = solvePositiveDefinite(L, b);
    std::cerr << "[Lu_0 - b]: " << (L * u0 - b).norm() << std::endl; // debugging

    // Apply shifts to recover u.
    CornerData<double> u(mesh);
    geom.requireVertexIndices();
    for (Vertex v : mesh.vertices()) {
        size_t idx = DOFindex[v];
        for (Corner c : v.adjacentCorners()) u[c] = u0[idx] + reducedCoordinates[c];
    }
    geom.unrequireVertexIndices();
    return u;
}

/*
 * Input: The interior endpoints of a curve Γ.
 *
 * Output: The Darboux derivative Du = ω ∈ R^|E|, represented as a discrete primal 1-form. Values of ω at edges incident
 * on interior endpoints are 0 by definition (Section 2.4.2.)
 */
Vector<double> SurfaceWindingNumbersSolver::DarbouxDerivative(const VertexData<bool>& isInteriorEndpoint,
                                                              const CornerData<double>& u) const {

    geom.requireEdgeIndices();
    Vector<double> omega = Vector<double>::Zero(mesh.nEdges());

    for (Edge e : mesh.edges()) {

        // For every edge connecting to a vertex at a curve endpoint, set the value to zero.
        if (isInteriorEndpoint[e.firstVertex()] || isInteriorEndpoint[e.secondVertex()]) continue;

        size_t i = geom.edgeIndices[e];
        Halfedge he = e.halfedge();
        Corner c1 = he.corner();
        Corner c2 = he.next().corner();
        Corner d2 = c2;
        Corner d1 = c1;
        if (!e.isBoundary()) {
            d2 = he.twin().corner();
            d1 = he.twin().next().corner();
        }
        omega[i] = 0.5 * ((u[c2] - u[c1]) + (u[d2] - u[d1]));
    }
    geom.unrequireEdgeIndices();
    return omega;
}

/*
 * Input: The interior endpoints of a curve Γ.
 *
 * Output: The standard cotan Laplacian on V^*, the set of vertices minus interior endpoints.
 */
SparseMatrix<double> SurfaceWindingNumbersSolver::buildLaplacian(const VertexData<bool>& isInteriorEndpoint,
                                                                 const VertexData<size_t>& DOFindex,
                                                                 const size_t& nDOFs) const {

    // Build Laplace matrix.
    SparseMatrix<double> L(nDOFs, nDOFs);
    std::vector<Eigen::Triplet<double>> tripletList;
    for (Face f : mesh.faces()) {

        for (Halfedge he : f.adjacentHalfedges()) {
            if (isInteriorEndpoint[he.tailVertex()] || isInteriorEndpoint[he.tipVertex()]) {
                continue;
            }

            size_t i = DOFindex[he.tailVertex()];
            size_t j = DOFindex[he.tipVertex()];
            double w = geom.halfedgeCotanWeights[he];
            if (std::isnan(w) || !std::isfinite(w)) w = 1;
            tripletList.emplace_back(i, i, w);
            tripletList.emplace_back(j, j, w);
            tripletList.emplace_back(i, j, -w);
            tripletList.emplace_back(j, i, -w);
        }
    }
    L.setFromTriplets(tripletList.begin(), tripletList.end());
    return L;
}

/*
 * Input: The interior vertices and interior endpoints of a curve Γ.
 *
 * Output: The r.h.s. |V^*|-vector encoding the jump conditions for the jump Laplace equation.
 */
Vector<double> SurfaceWindingNumbersSolver::buildJumpLaplaceRHS(const std::vector<Vertex>& interiorVertices,
                                                                const VertexData<bool>& isInteriorEndpoint,
                                                                const CornerData<double>& reducedCoordinates,
                                                                const VertexData<size_t>& DOFindex,
                                                                const size_t& nDOFs) const {

    // Build RHS (with DOFs at curve endpoints omitted)
    Vector<double> RHS = Vector<double>::Zero(nDOFs);
    for (const Vertex& v : interiorVertices) {
        for (Halfedge he : v.outgoingHalfedges()) {
            if (isInteriorEndpoint[he.tipVertex()]) continue;
            Halfedge heB = he.next().next();
            double cumJump = reducedCoordinates[he.corner()];
            double wA = geom.halfedgeCotanWeights[he];
            double wB = geom.halfedgeCotanWeights[heB];
            size_t vI = DOFindex[v];
            size_t vJ = DOFindex[he.tipVertex()];
            size_t vK = DOFindex[heB.tailVertex()];
            RHS[vI] -= wA * cumJump;
            RHS[vJ] += wA * cumJump;
            RHS[vI] -= wB * cumJump;
            RHS[vK] += wB * cumJump;
        }
    }
    return RHS;
}

/*
 * Input: A discrete primal 1-form ω ∈ R^|E| with no exact component.
 *
 * Output: The harmonic component γ ∈ R^|E| of ω, also a primal 1-form.
 */
Vector<double> SurfaceWindingNumbersSolver::harmonicComponent(const Vector<double>& omega) const {

    Vector<double> deltaBeta = computeCoExactComponent(omega);
    Vector<double> gamma = omega - deltaBeta;
    return gamma;
}

CornerData<double> SurfaceWindingNumbersSolver::residualFunction(const Vector<double>& chain,
                                                                 const Vector<double>& gamma) const {

    CornerData<double> vInit = integrateLocally(gamma);
    CornerData<double> v = solveLinearProgram(chain, vInit);
    return v;
}

CornerData<double> SurfaceWindingNumbersSolver::integrateLocally(const Vector<double>& gamma) const {

    CornerData<double> vInit(mesh, 0);
    geom.requireEdgeIndices();
    for (Face f : mesh.faces()) {
        for (Halfedge he : f.adjacentHalfedges()) {
            size_t eIdx = geom.edgeIndices[he.edge()];
            Corner cA = he.corner();
            Corner cB = he.next().corner();
            double diff = he.orientation() ? gamma[eIdx] : -gamma[eIdx];
            vInit[cB] = vInit[cA] + diff;
        }
    }
    geom.unrequireEdgeIndices();
    return vInit;
}

CornerData<double> SurfaceWindingNumbersSolver::solveLinearProgram(const Vector<double>& chain,
                                                                   const CornerData<double>& vInit) const {

    size_t F = mesh.nFaces();
    size_t E = mesh.nEdges();
    size_t numVars = F + E; // DOFs + slack variables

    // Set up environment
    std::cerr << "Setting up Gurobi environment..." << std::endl;
    GRBEnv env = GRBEnv();
    GRBModel model = GRBModel(env);

    // Allocate variables
    std::cerr << "Allocating variables and setting up objective..." << std::endl;
    std::vector<GRBVar> X(numVars);
    geom.requireEdgeLengths();
    for (size_t i = 0; i < F; i++) {
        X.push_back(model.addVar(-GRB_INFINITY, GRB_INFINITY, 0., GRB_CONTINUOUS)); // lb, ub, obj, vtype, vname=""
    }
    for (size_t i = 0; i < E; i++) {
        double cMag = abs(chain[i]);
        double coeff = (cMag < 1e-5) ? 1. : epsilon;
        // Add lower bound to enforce that the slack variables are positive.
        X.push_back(model.addVar(0., GRB_INFINITY, coeff * geom.edgeLengths[i], GRB_CONTINUOUS));
    }
    model.set(GRB_IntAttr_ModelSense, GRB_MINIMIZE);

    // Set up constraints
    std::cerr << "Setting up constraints..." << std::endl;
    geom.requireFaceIndices();
    for (size_t i = 0; i < E; i++) {
        // Compute the difference in face values across edge i
        Edge ei = mesh.edge(i);
        if (ei.isBoundary()) continue;
        Halfedge he = ei.halfedge();
        // Note: the below two lines assume a manifold mesh.
        size_t fA = geom.faceIndices[he.face()];
        size_t fB = geom.faceIndices[he.twin().face()];
        double diff = vInit[he.corner()] - vInit[he.twin().next().corner()];

        // Constraints to define the slack variables.
        model.addConstr(-X[F + i] <= diff + X[fA] - X[fB]); // -z - Cij*mu <= 0
        model.addConstr(X[F + i] >= diff + X[fA] - X[fB]);  // z - Cij*mu >= 0

        // Add constraint so that for each edge that was originally in the curve, the jump in v is at most jump in u
        // ("no extra loops" constraint).
        double gij = chain[i];
        model.addConstr(0 <= gij * (diff + X[fA] - X[fB]));         // Gij Cij*mu >= 0
        model.addConstr(gij * (diff + X[fA] - X[fB]) <= gij * gij); // Gij Cij*mu <= Gij * Gij
    }
    geom.unrequireFaceIndices();

    std::cerr << "Solving..." << std::endl;
    try {
        model.optimize();
        int optimstatus = model.get(GRB_IntAttr_Status);
        if (optimstatus == GRB_OPTIMAL) {
            std::cout << "Optimal objective: " << model.get(GRB_DoubleAttr_ObjVal) << std::endl;
        } else if (optimstatus == GRB_INFEASIBLE) {
            std::cout << "Model is infeasible" << std::endl;
        } else if (optimstatus == GRB_UNBOUNDED) {
            std::cout << "Model is unbounded" << std::endl;
        } else {
            std::cout << "Optimization was stopped with status = " << optimstatus << std::endl;
        }
    } catch (GRBException e) {
        std::cout << "Error code = " << e.getErrorCode() << std::endl;
        std::cout << e.getMessage() << std::endl;
    } catch (...) {
        std::cout << "Error during optimization" << std::endl;
    }

    // Reconstruct solution using shifts solved for from LP.
    CornerData<double> vPost = vInit; // v post-shift
    for (size_t i = 0; i < F; i++) {
        Face f = mesh.face(i);
        double shift = X[i].get(GRB_DoubleAttr_X);
        for (Corner c : f.adjacentCorners()) vPost[c] += shift;
    }

    return vPost;
}

CornerData<double>
SurfaceWindingNumbersSolver::approximateResidualFunction(const Vector<double>& inputChain,
                                                         const std::vector<std::pair<Vertex, bool>>& endpoints,
                                                         const Vector<double>& gamma) const {

    // Complete curve using a shortest-path heuristic.
    Vector<double> chain = dijkstraCompleteCurve(inputChain, endpoints);
    // polyscope::getSurfaceMesh("input mesh")->addEdgeScalarQuantity("Dijkstra-completed curve", completedChain);

    // Detect connected components.
    // In the same loop, integrate gamma within each connected component.
    double eps = 1e-5;
    int regionLabel = 0;
    FaceData<int> visitedFace(mesh, 0);
    CornerData<double> vInit(mesh, 0);
    geom.requireEdgeIndices();
    for (Face seedFace : mesh.faces()) {

        if (visitedFace[seedFace] > 0) continue;

        // Locally integrate within face.
        Halfedge he0 = seedFace.halfedge();
        Halfedge he1 = he0.next();
        Halfedge he2 = he1.next();
        vInit[he1.corner()] =
            he0.orientation() ? gamma[geom.edgeIndices[he0.edge()]] : -gamma[geom.edgeIndices[he0.edge()]];
        vInit[he2.corner()] = vInit[he1.corner()];
        vInit[he2.corner()] +=
            he1.orientation() ? gamma[geom.edgeIndices[he1.edge()]] : -gamma[geom.edgeIndices[he1.edge()]];

        // BFS
        regionLabel += 1;
        std::vector<Face> bag = {seedFace};
        while (bag.size() > 0) {
            Face currFace = bag.back();
            bag.pop_back();
            visitedFace[currFace] = regionLabel;
            for (Halfedge he : currFace.adjacentHalfedges()) {
                Face neighbor = he.twin().face();
                if (abs(chain[geom.edgeIndices[he.edge()]]) < eps && visitedFace[neighbor] == 0) {
                    bag.push_back(neighbor);
                    // Locally integrate within face, and match across edge.
                    he0 = he.twin().next();
                    he1 = he0.next();
                    he2 = he1.next();
                    vInit[he0.corner()] = vInit[he.corner()];
                    vInit[he1.corner()] = vInit[he0.corner()];
                    vInit[he1.corner()] += (he0.orientation() ? gamma[geom.edgeIndices[he0.edge()]]
                                                              : -gamma[geom.edgeIndices[he0.edge()]]);
                    vInit[he2.corner()] = vInit[he1.corner()];
                    vInit[he2.corner()] += (he1.orientation() ? gamma[geom.edgeIndices[he1.edge()]]
                                                              : -gamma[geom.edgeIndices[he1.edge()]]);
                }
            }
        }
    }
    geom.unrequireEdgeIndices();
    int nComponents = regionLabel;
    // polyscope::getSurfaceMesh("input mesh")->->addFaceScalarQuantity("connected components", visitedFace);
    // polyscope::getSurfaceMesh("input mesh")->->addCornerScalarQuantity("v pre-shift", vInit);

    EdgeData<size_t> bEdgeIdx(mesh, 0); // dense re-indexing of edges on component boundaries
    std::vector<Edge> bEdges;
    size_t reIdx = 0;
    for (Edge e : mesh.edges()) {
        if (e.isBoundary()) continue;
        Halfedge he = e.halfedge();
        if (visitedFace[he.face()] != visitedFace[he.twin().face()]) {
            bEdgeIdx[e] = reIdx;
            bEdges.push_back(e);
            reIdx++;
        }
    }


    // Run reduced-size LP, where [# of DOFs] = [# of connected components].
    size_t numVars = nComponents + reIdx; // DOFs + slack variables
    std::cerr << "Setting up Gurobi environment..." << std::endl;
    GRBEnv env = GRBEnv();
    GRBModel model = GRBModel(env);

    // Allocate variables
    std::cerr << "Allocating variables and setting up objective..." << std::endl;
    std::vector<GRBVar> X(numVars);
    geom.requireEdgeLengths();
    for (size_t i = 0; i < nComponents; i++) {
        X.push_back(model.addVar(-GRB_INFINITY, GRB_INFINITY, 0., GRB_CONTINUOUS));
    }
    for (size_t i = 0; i < reIdx; i++) {
        // Add lower bound to enforce that the slack variables are positive.
        X.push_back(model.addVar(0., GRB_INFINITY, geom.edgeLengths[bEdges[i]], GRB_CONTINUOUS));
    }
    model.set(GRB_IntAttr_ModelSense, GRB_MINIMIZE);

    // Set up constraints
    std::cerr << "Setting up constraints..." << std::endl;
    geom.requireFaceIndices();
    for (size_t i = 0; i < reIdx; i++) {
        Edge e = bEdges[i];
        if (e.isBoundary()) continue;
        Halfedge he = e.halfedge();
        Corner c0 = he.corner();
        Corner c1 = he.next().corner();
        Corner d0 = he.twin().next().corner();
        Corner d1 = he.twin().next().next().next().corner();
        int r0 = visitedFace[he.face()] - 1;
        int r1 = visitedFace[he.twin().face()] - 1;
        double diff = 0.5 * ((vInit[c0] - vInit[d0]) + (vInit[c1] - vInit[d1])); // average diff

        // Constraints to define the slack variables.
        model.addConstr(-X[nComponents + i] <= diff + X[r0] - X[r1]);
        model.addConstr(X[nComponents + i] >= diff + X[r0] - X[r1]);
    }
    geom.unrequireFaceIndices();

    std::cerr << "Solving..." << std::endl;
    try {
        model.optimize();
        int optimstatus = model.get(GRB_IntAttr_Status);
        if (optimstatus == GRB_OPTIMAL) {
            std::cout << "Optimal objective: " << model.get(GRB_DoubleAttr_ObjVal) << std::endl;
        } else if (optimstatus == GRB_INFEASIBLE) {
            std::cout << "Model is infeasible" << std::endl;
        } else if (optimstatus == GRB_UNBOUNDED) {
            std::cout << "Model is unbounded" << std::endl;
        } else {
            std::cout << "Optimization was stopped with status = " << optimstatus << std::endl;
        }
    } catch (GRBException e) {
        std::cout << "Error code = " << e.getErrorCode() << std::endl;
        std::cout << e.getMessage() << std::endl;
    } catch (...) {
        std::cout << "Error during optimization" << std::endl;
    }

    // Reconstruct solution using shifts solved for from LP.
    CornerData<double> vPost = vInit; // v post-shift
    for (size_t i = 0; i < nComponents; i++) {
        double shift = X[i].get(GRB_DoubleAttr_X);
        for (Face f : mesh.faces()) {
            if (visitedFace[f] != i + 1) continue;
            for (Corner c : f.adjacentCorners()) vPost[c] += shift;
        }
    }

    return vPost;
}

Vector<double>
SurfaceWindingNumbersSolver::dijkstraCompleteCurve(const Vector<double>& chain,
                                                   const std::vector<std::pair<Vertex, bool>>& curveEndpoints) const {

    // Create custom EdgeLengthGeometry, removing original curve edges from the graph.
    EdgeData<double> dijkstraWeights(mesh);
    geom.requireEdgeLengths();
    geom.requireEdgeIndices();
    const double infinity = std::numeric_limits<double>::infinity();
    for (Edge e : mesh.edges()) {
        size_t eIdx = geom.edgeIndices[e];
        double length = (abs(chain[eIdx]) > 1e-5) ? geom.edgeLengths[e] : infinity;
        dijkstraWeights[e] = length;
    }
    geom.unrequireEdgeLengths();
    geom.unrequireEdgeIndices();
    EdgeLengthGeometry dijkstraMesh(mesh, dijkstraWeights);

    // Only connect endpoints of opposite sign.
    Vertex startVert;
    bool sgn;
    std::vector<std::pair<Vertex, bool>> endpoints = curveEndpoints;
    std::vector<Halfedge> completedCurve;
    while (endpoints.size() > 0) {
        std::tie(startVert, sgn) = endpoints.back();
        endpoints.pop_back();
        std::set<Vertex> endVerts;
        for (auto& tup : endpoints) {
            if (tup.second != sgn) endVerts.insert(tup.first);
        }
        std::vector<Halfedge> path = dijkstraPath(dijkstraMesh, startVert, endVerts);
        Vertex endVert = path.back().tipVertex();
        if (!sgn) {
            std::reverse(path.begin(), path.end());
            for (Halfedge& he : path) {
                he = he.twin();
            }
        }
        completedCurve.insert(completedCurve.end(), path.begin(), path.end());
        // Determine which endpoint we ended at, so we can delete it.
        for (size_t i = 0; i < endpoints.size(); i++) {
            if (endpoints[i].first == endVert) {
                endpoints.erase(endpoints.begin() + i);
                break;
            }
        }
    }

    // Convert to chain.
    Vector<double> completedChain = chain + convertToChain(completedCurve);

    return completedChain;
}

/*
 * Copied from geometry-central with a minor change to the termination condition so it can take in multiple possible
 * endpoints.
 */
std::vector<Halfedge> SurfaceWindingNumbersSolver::dijkstraPath(IntrinsicGeometryInterface& geom_,
                                                                const Vertex& startVert,
                                                                const std::set<Vertex>& endVerts) const {

    // Early out for empty case
    if (endVerts.find(startVert) != endVerts.end()) {
        return std::vector<Halfedge>();
    }

    // Gather values
    SurfaceMesh& mesh = geom_.mesh;
    geom_.requireEdgeLengths();

    // Search state: incoming halfedges to each vertex, once discovered
    std::unordered_map<Vertex, Halfedge> incomingHalfedge;

    // Search state: visible neighbors eligible to expand to
    using WeightedHalfedge = std::tuple<double, Halfedge>;
    std::priority_queue<WeightedHalfedge, std::vector<WeightedHalfedge>, std::greater<WeightedHalfedge>> pq;

    // Helper to add a vertex's
    auto vertexDiscovered = [&](Vertex v) {
        return v == startVert || incomingHalfedge.find(v) != incomingHalfedge.end();
    };
    auto enqueueVertexNeighbors = [&](Vertex v, double dist) {
        for (Halfedge he : v.outgoingHalfedges()) {
            if (!vertexDiscovered(he.twin().vertex())) {
                double len = geom_.edgeLengths[he.edge()];
                double targetDist = dist + len;
                pq.emplace(targetDist, he);
            }
        }
    };

    // Add initial halfedges
    enqueueVertexNeighbors(startVert, 0.);

    while (!pq.empty()) {

        // Get the next closest neighbor off the queue
        double currDist = std::get<0>(pq.top());
        Halfedge currIncomingHalfedge = std::get<1>(pq.top());
        pq.pop();

        Vertex currVert = currIncomingHalfedge.twin().vertex();
        if (vertexDiscovered(currVert)) continue;

        // Accept the neighbor
        incomingHalfedge[currVert] = currIncomingHalfedge;

        // Found path! Walk backwards to reconstruct it and return
        if (endVerts.find(currVert) != endVerts.end()) {
            std::vector<Halfedge> path;
            Vertex walkV = currVert;
            while (walkV != startVert) {
                Halfedge prevHe = incomingHalfedge[walkV];
                path.push_back(prevHe);
                walkV = prevHe.vertex();
            }

            std::reverse(std::begin(path), std::end(path));

            geom_.unrequireEdgeLengths();
            return path;
        }

        // Enqueue neighbors
        enqueueVertexNeighbors(currVert, currDist);
    }

    // Didn't find path
    geom_.unrequireEdgeLengths();
    return std::vector<Halfedge>();
}

/*
 * Input: The interior endpoints of curve Γ, residual function v on corners, and reduced coordinates associated with Γ.
 *
 * Output: Updated reduced coordinates encoding new jump constraints for the jump Laplace equation.
 */
CornerData<double> SurfaceWindingNumbersSolver::subtractJumpDerivative(
    const Vector<double>& chain, const std::vector<Vertex>& interiorVertices,
    const VertexData<bool>& isInteriorEndpoint, const std::map<Vertex, Halfedge>& outgoingHalfedgeOnCurve,
    const CornerData<double>& resid) const {

    // Vector<double> updatedChain = chain;
    // for (const auto& vi : interiorVertices) {
    //     //
    // }
    // CornerData<double> c = computeReducedCoordinates(updatedChain, interiorVertices, outgoingHalfedgeOnCurve);
    // return c;

    geom.requireEdgeIndices();
    CornerData<double> reducedCoordinates(mesh, 0);
    for (const auto& vi : interiorVertices) {
        // we should have already filtered for nonmanifold vertices when we computed interiorVertices
        Halfedge start = outgoingHalfedgeOnCurve.at(vi);
        Halfedge curr = start;
        double cumJump = 0.; // cumulative jump
        do {
            if (curr.isInterior()) {
                if (!curr.edge().isBoundary()) {
                    // always have a jump of 0 across edges on the boundary
                    double Ju = chain[geom.edgeIndices[curr.edge()]];
                    if (!curr.orientation()) Ju = -Ju;
                    double Jv = resid[curr.corner()] - resid[curr.twin().next().corner()];
                    double jump = Ju - Jv;
                    cumJump += jump;
                }
                reducedCoordinates[curr.corner()] = cumJump;
            }
            curr = curr.next().next().twin(); // go counterclockwise
        } while (curr != start);
    }
    geom.unrequireEdgeIndices();
    return reducedCoordinates;
}


// ==== AUXILIARY FUNCTIONS


/*
 * Build the solvers for performing Hodge decomposition on a primal 1-form.
 *
 * The output of this function only remains valid if the mesh remains un-mutated! (which is ensured since the mesh, etc.
 * variables are private; if these change, the user must re-construct a new SurfaceWindingNumbersSolver.)
 */
void SurfaceWindingNumbersSolver::ensureHaveCoexactSolver() {

    if (coexactSolver == nullptr) {

        // geom.requireEdgeIndices();
        // geom.requireEdgeCotanWeights();
        // const EdgeData<size_t>& eIdx = geom.edgeIndices;
        // for (Edge e : mesh.edges()) {
        //     double wInv = geom.edgeCotanWeights[e];
        //     double w = 1. / wInv;
        //     if (!std::isfinite(w) || std::isnan(w) || w < 0) {
        //         w = 1;
        //     } else if (w > 1000) {
        //         w = 1000;
        //     }
        //     hodge1Inv.coeffRef(eIdx[e], eIdx[e]) = w;
        // }
        // geom.unrequireEdgeIndices();
        // geom.unrequireEdgeCotanWeights();

        SparseMatrix<double> B = d1 * hodge1Inv * d1T;
        shiftDiagonal(B, 1e-8);
        coexactSolver.reset(new PositiveDefiniteSolver<double>(B));
    }
}

/*
 * Compute the 2-form potential β by solving the system d𝛿β = dω.
 *
 * Returns the coexact component 𝛿β of ω, as a primal 1-form.
 */
Vector<double> SurfaceWindingNumbersSolver::computeCoExactComponent(const Vector<double>& omega) const {

    Vector<double> rhs = d1 * omega;
    Vector<double> betaTilde = coexactSolver->solve(rhs);
    return hodge1Inv * d1T * betaTilde;
}


// ==== UTILITIES

Vector<double> SurfaceWindingNumbersSolver::convertToChain(const std::vector<Halfedge>& curve) const {

    Vector<double> chain = Vector<double>::Zero(mesh.nEdges());
    geom.requireEdgeIndices();
    for (const Halfedge& he : curve) {
        size_t eIdx = geom.edgeIndices[he.edge()];
        chain[eIdx] += he.orientation() ? 1 : -1;
    }
    geom.unrequireEdgeIndices();
    return chain;
}

Vector<double> SurfaceWindingNumbersSolver::convertToChain(const std::vector<Vertex>& curve) const {

    Vector<double> chain = Vector<double>::Zero(mesh.nEdges());
    geom.requireEdgeIndices();
    int N = curve.size();
    for (int i = 0; i < N - 1; i++) {
        Halfedge he = determineHalfedgeFromVertices(curve[i], curve[(i + 1) % N]);
        size_t eIdx = geom.edgeIndices[he.edge()];
        chain[eIdx] += he.orientation() ? 1 : -1;
    }
    geom.unrequireEdgeIndices();
    return chain;
}


// ==== CONSTRUCTOR


SurfaceWindingNumbersSolver::SurfaceWindingNumbersSolver(IntrinsicGeometryInterface& geom_)
    : mesh(geom_.mesh), geom(geom_) {

    if (!mesh.isTriangular()) throw std::logic_error("Mesh must be triangular to run SWN.");

    // DEC operators
    // TODO: mollify hodge1 weights / halfedgeCotanWeights?
    geom.requireDECOperators();
    d0 = geom.d0;
    d0T = d0.transpose();
    hodge1 = geom.hodge1;
    hodge1Inv = geom.hodge1Inverse;
    d1 = geom.d1;
    d1T = d1.transpose();
    geom.unrequireDECOperators();

    // Create solvers
    ensureHaveCoexactSolver();

    // Determine whether the mesh is simply-connected.
    simplyConnected = false;
    if (mesh.isManifold() && mesh.nConnectedComponents() == 1) {
        size_t chi = mesh.nVertices() + mesh.nFaces() - mesh.nEdges();
        simplyConnected = (chi == 1 || chi == 2);
    }
}