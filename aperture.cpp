#include "aperture.h"
        
Aperture::Aperture(double radius, double secondaryRadius, int sampleRate) {
    this->radius = radius;
    this->secRadius = secondaryRadius;
    this->sampleRate = sampleRate;
}

Aperture::~Aperture() {
    // TODO: remove/de-init stuff....
}

point_2d Aperture::sampleDisk(double minRadius) {
    double dr = 1/this->sampleRate;

    std::vector<double> x;
    std::vector<double> y;

    // TODO: add sampling logic; figure out HOW TO replicate MATLAB's 'linspace'
    // Potentially use: `Eigen::ArrayXf::LinSpaced` or the function `linspace` in helpers.h

    std::vector<double> radii = helpers::linspace(minRadius, 1, this->sampleRate);
    int k = ceil(minRadius*(this->sampleRate + 1));

    if (minRadius == 0) {
        x.push_back(0);
        y.push_back(0);

        // Overwrite the radii and k parameters
        radii = helpers::linspace(dr, 1, this->sampleRate);
        k = 1;
    }

    for (const auto& radius : radii) {
        int n = round(M_PI/asin(1/(2*static_cast<double>(k))));

        std::vector<double> theta = helpers::linspace(0, 2*M_PI, n + 1);

        std::vector<double> x_r;
        std::vector<double> y_r;

        std::for_each(theta.begin(), theta.end(), [radius, &x_r, &y_r](double t) {
            x_r.push_back(radius * cos(t));
            y_r.push_back(radius * sin(t));
        });
        
        // TODO: look @ possible overhead of `reserve`!?
        x.reserve(x.size() + x_r.size());
        x.insert(x.end(), x_r.begin(), x_r.end());
        
        y.reserve(y.size() + y_r.size());
        y.insert(y.end(), y_r.begin(), y_r.end());

        ++k;
    }

    return std::make_pair(x, y);
}

Eigen::Transform<double, 3, Eigen::Affine> Aperture::transform(double hourAngle, double declination) {
    // Creating the base to primary aperture transformation matrix 
    Eigen::Transform <double, 3, Eigen::Affine> H = Eigen::Transform <double, 3, Eigen::Affine>::Identity();

    // Frame 0 -> 1: pier to RA axis
    H.translate(Eigen::Vector3d(0.0, 0.0, cfg::LENGTH_1));
    
    // Frame 1 -> 2: RA axis to Dec axis
    H.rotate(Eigen::AngleAxisd(helpers::radians(90-cfg::LATITUDE), Eigen::Vector3d::UnitX()));
    H.rotate(Eigen::AngleAxisd(helpers::radians(-hourAngle), Eigen::Vector3d::UnitZ()));
    H.translate(Eigen::Vector3d(0.0, 0.0, cfg::LENGTH_2));
    
    // Frame 2 -> 3: Dec axis to optical axis
    H.rotate(Eigen::AngleAxisd(helpers::radians(declination), Eigen::Vector3d::UnitX()));
    H.translate(Eigen::Vector3d(-cfg::LENGTH_3, 0.0, 0.0));

    return H;
}

Eigen::MatrixXd Aperture::sampleAperture(double hourAngle, double declination, std::vector<double> x, std::vector<double> z) {
    Eigen::MatrixXd aperture(3, x.size());

    aperture.row(0) = Eigen::VectorXd::Map(x.data(), x.size());
    aperture.row(2) = Eigen::VectorXd::Map(z.data(), z.size());

    Eigen::Transform<double, 3, Eigen::Affine> pose;
    pose = this->transform(hourAngle, declination);

    // Compute the aperture points in the dome frame
    Eigen::MatrixXd apertureInDome(4, x.size());
    apertureInDome = pose * aperture.colwise().homogeneous(); // Maybe need to use `pose.matrix()` instead of `pose`

    return apertureInDome;
}

Eigen::Vector3d Aperture::apertureDirection(double hourAngle, double declination)  {
    Eigen::Transform<double, 3, Eigen::Affine> H = this->transform(hourAngle, declination);

    Eigen::Transform<double, 3, Eigen::Affine> unitH = H;
    unitH.translate(Eigen::Vector3d(0.0, 1.0, 0.0));

    Eigen::MatrixXd pose = H.matrix() * unitH.matrix() - H.matrix();
    
    Eigen::Vector3d origin = Eigen::Vector3d::Zero();
    Eigen::Vector4d d = pose * origin.homogeneous();

    return d.head<3>(); // homogeneous -> affine coordinates
}

bool Aperture::isRayBlocked(Eigen::Vector3d& origin, double hourAngle, double declination, double domeAzimuth) {
    bool isBlocked = true;

    // Get the ray's direction
    Eigen::Vector3d direction;
    direction = this->apertureDirection(hourAngle, declination);
    
    try {
        Ray castRay(origin, direction);
        bool hasIntersection = castRay.findIntersection();

        if (hasIntersection) {
            Eigen::Vector3d point = castRay.getIntersection();

            double azCorrected = std::fmod(domeAzimuth - 180.0, 360.0);

            Eigen::Transform <double, 3, Eigen::Affine> rot = Eigen::Transform <double, 3, Eigen::Affine>::Identity();
            rot.rotate(Eigen::AngleAxisd(helpers::radians(azCorrected), Eigen::Vector3d::UnitZ()));

            Eigen::Vector4d originTransformed = rot.matrix() * point.colwise().homogeneous();

            double r = 0.5 * cfg::DOME_DIAMETER * sin(helpers::radians(15));

            bool xCondition = -cfg::DOME_SLIT_WIDTH/2 < originTransformed(0) && originTransformed(0) < cfg::DOME_SLIT_WIDTH/2;
            bool yCondition = -r < originTransformed(1) && originTransformed(1) < 0.5 * cfg::DOME_DIAMETER;

            if (point(2) > cfg::DOME_EXTENT && xCondition && yCondition) {
                isBlocked = false;
            }
        }
    } catch (std::exception& ex) {
        std::cout << "Error occured..!" << std::endl;
    }

    return isBlocked;
}

std::vector<bool> Aperture::isBlocked(Eigen::MatrixXd& origins, double hourAngle, double declination, double domeAzimuth) {
    std::vector<bool> blocked;
    
    int n = origins.cols(); // no. points in the aperture

    for (int i = 0; i < n; ++i) {
        Eigen::Vector3d origin = origins.col(i);
        bool b = this->isRayBlocked(origin, hourAngle, declination, domeAzimuth);

        blocked.push_back(b);
    }

    return blocked;
}

double Aperture::obstruction(double hourAngle, double declination, double domeAzimuth) {
    double ratio = 0;

    point_2d xz = this->sampleDisk(); // TODO: add min radius
    std::vector<double> x = std::get<0>(xz);
    std::vector<double> z = std::get<1>(xz);    
    
    // Negate the entries of `x`
    std::vector<double> xNeg;
    xNeg.resize(x.size());

    double neg = -1.0;

    std::transform(x.begin(), x.end(),  xNeg.begin(), [neg](double value) {
        return neg * value;
    });

    Eigen::MatrixXd apPosition = this->sampleAperture(hourAngle, declination, xNeg, z);

    // Compute blocked rays
    std::vector<bool> blocked = this->isBlocked(apPosition, hourAngle, declination, domeAzimuth);

    // // Compute the ratio
    // ratio = std::count(blocked.begin(), blocked.end(), true)/blocked.size();

    return ratio;
}