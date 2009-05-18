%% Declare boat constants

% Initial conditions

e_0 = 0;                % Initial east position
n_0 = 0;                % Initial north position
phi_0 = 0;              % Initial heading
sea_state = 0;          % Not sure, pulled from m_sea_state in BoatSimulator.cpp
T_step = 0.01;          % Simulation timestep

% Known constants

rho_w = 1.03e3;         % Density of saltwater
d_prop = .4572;         % Propeller diameter:18" (m)
mass = 550;             % Boat mass (kg)
wheelbase = 3.27;       % Distance from mid-keel to mid-rudder (m)
m_battery = 108.86;     % Weight of ballast batteries (kg)
d_battery = .3048;      % Distance of batteries from rotation axis (m)
disp_vessel = 612.35;   % Vessel displacement:1350 lbs (kg)
max_batt_angle = 1.309; % Maximum angle for the battery tray: 75 degrees(radians)
max_sea_state = 12;     % Relates to sea_state initial condition. Not sure of this variable... TODO: Figure this out
rudder_angle_max = 1;   % Maximum angle of the rudder (radians)

% Experimental constants

K_t = 0.5;              % Thrust coefficient
C_d_sea_surge = 340;    % Coefficient of drag for forward motion through water
cg_x = .0508;           % Center of gravity relative to boat center: 1/6' (m)
cg_z = .0508;           % Center of gravity relative to boat center: 1/6' (m)
% Boat center is at 6.5' by the waterline by the axis of symmetry
rudder_angle_dot_max = 10;   % Maximum rate of change of rudder angle (radians/s)
proportional_band = .5;      % Proportional band of the rudder response (radians)
roll_period = 3;        % Periodicity of roll. From BoatSimulator.cpp. (s)