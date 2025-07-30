import networkx as nx
from collections import deque
import walker
from sentence_transformers import SentenceTransformer, util
import copy
import random
from operator import attrgetter
import numpy as np
from scipy.spatial.distance import cosine
from scipy.stats import beta
import time
from typing import List, Tuple, Dict, Any
from dataclasses import dataclass
import heapq

node_similarity_threshold = 0.8
relation_similarity_threshold = 0.4

encoder = SentenceTransformer('all-MiniLM-L6-v2') # Model to create embeddings

@dataclass
class EventUtilityFeatures:
    """Features used for probabilistic utility prediction"""
    temporal_recency: float  # How recent is this event
    semantic_relevance: float  # Semantic similarity to query/target
    structural_importance: float  # Graph centrality metrics
    frequency_score: float  # How often this type of event occurs
    downstream_impact: float  # Potential impact on downstream processing
    
@dataclass
class PartialMatchFeatures:
    """Features for partial match (PM) importance"""
    completion_progress: float  # How close the PM is to completion (0-1)
    match_quality: float  # Quality/confidence of current partial match
    state_complexity: float  # Complexity of current DFA state
    path_length: float  # Current path length normalized
    acceptance_probability: float  # Probability this PM will be accepted
    
@dataclass 
class SheddingConfig:
    """Configuration for shedding algorithm"""
    latency_bound_ms: float = 1000.0
    utility_threshold: float = 0.3
    max_queue_size: int = 10000
    probabilistic_alpha: float = 2.0  # Beta distribution parameter
    probabilistic_beta: float = 5.0   # Beta distribution parameter
    adaptation_rate: float = 0.1      # Learning rate for threshold adaptation
    event_pm_weight: float = 0.6      # Weight for combining event and PM utilities
    window_size: int = 100            # Window size for temporal considerations

def get_similarity_score(lhs, rhs):
    return util.cos_sim(lhs, rhs)

def node_similarity_function(node, embedding):
    return util.cos_sim(node['embedding'], embedding) > node_similarity_threshold

def edge_similarity_function(edge, embedding):
    return util.cos_sim(edge['embedding'], embedding) > relation_similarity_threshold
    
class DFA_State:
    def __init__(self, idx=None, name=None):
        self.idx = idx
        self.name = name
        self.name_embedding = encoder.encode(name) if name else None
        
    def __hash__(self) -> int:
        return self.idx
    
    def __eq__(self, other) -> bool:
        if isinstance(other, DFA_State):
            return self.idx == other.idx
        return False
        
    def __repr__(self) -> str:
        return f"State with name {self.name}"


class DFA_Edge:
    def __init__(self, relation=None):
        self.relation = relation
        self.relation_embedding = encoder.encode(relation) if relation else None
    
    def __hash__(self) -> int:
        return hash(self.relation)
    
    def __eq__(self, other) -> bool:
        if isinstance(other, DFA_Edge):
            return self.relation == other.relation
        return False
        
    def __repr__(self) -> str:
        return f"Edge with relation {self.relation}"

class DFA:
    def __init__(self):
        self.transitions = {}
        self.start_state = None
        self.current_state = None
        self.accept_states = set()
        self.str = ""

    def add_state(self, state, is_start=False, is_accept=False):
        if state not in self.transitions:
            self.transitions[state] = {}
        if is_start:
            self.start_state = state
        if is_accept:
            self.accept_states.add(state)

    def add_transition(self, from_state, to_state, edge):
        if from_state not in self.transitions:
            self.transitions[from_state] = {}
        self.transitions[from_state][edge] = to_state

    def process_pattern(self, pattern: list, start_name="init"):
        self.str = ", ".join(pattern)
        self.current_state = DFA_State(0, start_name)
        self.add_state(self.current_state, is_start=True)
        current_state = self.current_state

        for idx, token in enumerate(pattern):
            if '|' in token:
                options = token.split('|')
                for option in options:
                    next_state = DFA_State(idx + 1)
                    relation = DFA_Edge(option) 
                    self.add_state(next_state, is_accept=(idx+1 == len(pattern)))
                    self.add_transition(current_state, next_state, relation)
                current_state = next_state
            else:
                next_state = DFA_State(idx + 1)
                self.add_state(next_state, is_accept=(idx+1 == len(pattern)))
                _token = token.rstrip('+')
                relation = DFA_Edge(_token)
                self.add_transition(current_state, next_state, relation)
                current_state = next_state
                if token.endswith('+'):
                    # add edge connecting itself
                    self.add_transition(current_state, next_state, relation)

    def recognize(self, sequence):
        current_state = self.start_state

        for token in sequence:
            if current_state not in self.transitions or token not in self.transitions[current_state]:
                return False
            current_state = self.transitions[current_state][token]

        return current_state in self.accept_states
    
    def process_one_token(self, token):
        if token in self.transitions[self.current_state]:
            self.current_state = self.transitions[self.current_state][token]
            
    def __repr__(self) -> str:
        return self.str
    
        
        
class Path:
    def __init__(self, start_node, dfa: DFA=None, tuples=[]) -> None:
        # accumulated scores according to different DFA, hardcode its dimension to be 3
        self.start_node = start_node
        self.dfa = dfa
        # path tuples containing (e,r,e) => [(e,r,e), ...] 
        self.tuples = tuples
        self.validity = True
        self.visited_nodes = {self.start_node}
        self.score = 1

    def add_new_tuple(self, new_tuple):
        new_path = copy.deepcopy(self)
        new_path.tuples.append(new_tuple)
        _, relation, next_node = new_tuple
        rel_embedding = relation['embedding']
        for dfa_relation in new_path.dfa.transitions[new_path.dfa.current_state]:
            similarity_score = get_similarity_score(dfa_relation.relation_embedding, rel_embedding)
            if similarity_score > relation_similarity_threshold:
                new_path.dfa.process_one_token(dfa_relation)
                new_path.score = new_path.score * 0.4 + similarity_score * 0.6
                new_path.visited_nodes.add(next_node)
        return new_path
    
    def if_accept_rel_embedding(self, rel_embedding):
        for relation in self.dfa.transitions[self.dfa.current_state]:
            if get_similarity_score(relation.relation_embedding, rel_embedding) > relation_similarity_threshold:
                return True
        return False
        
    def if_valid(self) -> bool:
        return self.validity
    
    def is_accepted(self) -> bool:
        if self.if_valid():
            if self.dfa.current_state in self.dfa.accept_states:
                return True
        return False
    
    def __len__(self):
        if self.tuples:
            return len(self.tuples)
        else:
            return 0
            
    def __repr__(self) -> str:
        result = ""
        for i, tuple in enumerate(self.tuples):
            if i == 0:
                h, r, t = tuple
                result += f"{h} -> {r['relation']} -> {t}"
            else:
                _, r, t = tuple
                result += f"-> {r['relation']} -> {t}"
        return result.strip()
        

def build_graph(kb_file_path) -> nx.Graph:
    G = nx.Graph()
    with open(kb_file_path, 'r') as file:
        for line in file:
            parts = line.strip().split('|')
            if len(parts) == 3:
                h, r, t = parts
                if h not in G:
                    h_embedding = encoder.encode(h)
                    G.add_node(h, embedding=h_embedding)
                if t not in G:
                    t_embedding = encoder.encode(t)
                    G.add_node(t, embedding=t_embedding)
                relation= r.strip()
                relation_embedding = encoder.encode(relation)
                G.add_edge(h, t, relation=relation, embedding=relation_embedding)
    return G


def get_truth_paths(q_entity: list, a_entity: list, graph: nx.Graph) -> list:
    '''
    Get shortest paths connecting question and answer entities.
    '''
    # Select paths
    paths = []
    for h in q_entity:
        if h not in graph:
            continue
        for t in a_entity:
            if t not in graph:
                continue
            try:
                for p in nx.all_shortest_paths(graph, h, t):
                    paths.append(p)
            except:
                pass
    # Add relation to paths
    result_paths = []
    for p in paths:
        tmp = []
        for i in range(len(p)-1):
            u = p[i]
            v = p[i+1]
            tmp.append((u, graph[u][v]['relation'], v))
        result_paths.append(tmp)
    return result_paths
    
def get_simple_paths(q_entity: list, a_entity: list, graph: nx.Graph, hop=2) -> list:
    '''
    Get all simple paths connecting question and answer entities within given hop
    '''
    # Select paths
    paths = []
    for h in q_entity:
        if h not in graph:
            continue
        for t in a_entity:
            if t not in graph:
                continue
            try:
                for p in nx.all_simple_edge_paths(graph, h, t, cutoff=hop):
                    paths.append(p)
            except:
                pass
    # Add relation to paths
    result_paths = []
    for p in paths:
        result_paths.append([(e[0], graph[e[0]][e[1]]['relation'], e[1]) for e in p])
    return result_paths

def get_negative_paths(q_entity: list, a_entity: list, graph: nx.Graph, n_neg: int, hop=2) -> list:
    '''
    Get negative paths for question witin hop
    '''
    # sample paths
    start_nodes = []
    end_nodes = []
    node_idx = list(graph.nodes())
    for h in q_entity:
        if h in graph:
            start_nodes.append(node_idx.index(h))
    for t in a_entity:
        if t in graph:
            end_nodes.append(node_idx.index(t))
    paths = walker.random_walks(graph, n_walks=n_neg, walk_len=hop, start_nodes=start_nodes, verbose=False)
    # Add relation to paths
    result_paths = []
    for p in paths:
        tmp = []
        # remove paths that end with answer entity
        if p[-1] in end_nodes:
            continue
        for i in range(len(p)-1):
            u = node_idx[p[i]]
            v = node_idx[p[i+1]]
            tmp.append((u, graph[u][v]['relation'], v))
        result_paths.append(tmp)
    return result_paths

def get_random_paths(q_entity: list, graph: nx.Graph, n=3, hop=2) -> tuple [list, list]:
    '''
    Get negative paths for question witin hop
    '''
    # sample paths
    start_nodes = []
    node_idx = list(graph.nodes())
    for h in q_entity:
        if h in graph:
            start_nodes.append(node_idx.index(h))
    paths = walker.random_walks(graph, n_walks=n, walk_len=hop, start_nodes=start_nodes, verbose=False)
    # Add relation to paths
    result_paths = []
    rules = []
    for p in paths:
        tmp = []
        tmp_rule = []
        for i in range(len(p)-1):
            u = node_idx[p[i]]
            v = node_idx[p[i+1]]
            tmp.append((u, graph[u][v]['relation'], v))
            tmp_rule.append(graph[u][v]['relation'])
        result_paths.append(tmp)
        rules.append(tmp_rule)
    return result_paths, rules

def construct_combined_DFA(regs: list) -> list[DFA]:
    assert len(regs) <= 3, "we only support at most 3 RPQs at the time"
    dfas= []
    for reg in regs:
        dfa = DFA()
        dfa.process_pattern(reg)
        dfas.append(dfa)
    # print(f"dfas are {dfas[0]} || {dfas[1]} || {dfas[2]}.")
    return dfas

class CombinedUtilityPredictor:
    """Enhanced probabilistic model combining event importance and partial match importance"""
    
    def __init__(self, config: SheddingConfig):
        self.config = config
        self.historical_utilities = []
        self.processing_times = []
        self.event_weights = np.array([0.25, 0.25, 0.2, 0.15, 0.15])  # Event feature weights
        self.pm_weights = np.array([0.3, 0.25, 0.2, 0.15, 0.1])      # PM feature weights
        self.beta_dist = beta(config.probabilistic_alpha, config.probabilistic_beta)
        self.event_window = []  # Sliding window for temporal analysis
        self.pm_states = {}     # Track partial match states
        
    def extract_event_features(self, node: str, graph: nx.Graph, 
                              query_context: Dict = None, position_in_window: int = 0) -> EventUtilityFeatures:
        """Extract event-specific features considering type and position in window"""
        node_data = graph.nodes[node]
        node_embedding = node_data['embedding']
        
        # Temporal recency based on position in window
        temporal_recency = max(0, 1.0 - (position_in_window / max(1, self.config.window_size)))
        
        # Semantic relevance to query context
        semantic_relevance = 0.5
        if query_context and 'target_embedding' in query_context:
            semantic_relevance = float(util.cos_sim(node_embedding, query_context['target_embedding']).item())
        
        # Structural importance (degree centrality)
        degree = graph.degree(node)
        max_degree = max(dict(graph.degree()).values()) if graph.nodes() else 1
        structural_importance = degree / max_degree
        
        # Event type frequency score
        frequency_score = self._calculate_event_type_frequency(node, graph)
        
        # Downstream impact
        try:
            downstream_impact = nx.clustering(graph, node)
        except:
            downstream_impact = 0.5
        
        return EventUtilityFeatures(
            temporal_recency=temporal_recency,
            semantic_relevance=semantic_relevance,
            structural_importance=structural_importance,
            frequency_score=frequency_score,
            downstream_impact=downstream_impact
        )
    
    def extract_pm_features(self, path: 'Path', max_hop: int = 3) -> PartialMatchFeatures:
        """Extract partial match features considering PM state and progress"""
        if not path or not path.dfa:
            return PartialMatchFeatures(0.5, 0.5, 0.5, 0.5, 0.1)
        
        # Completion progress: how close to acceptance
        total_states = len(path.dfa.accept_states) + len(path.dfa.transitions)
        current_state_distance = self._calculate_state_distance_to_acceptance(path)
        completion_progress = max(0, 1.0 - (current_state_distance / max(1, total_states)))
        
        # Match quality based on path score
        match_quality = min(1.0, max(0.0, path.score))
        
        # State complexity: number of possible transitions from current state
        state_complexity = 0.5
        if path.dfa.current_state in path.dfa.transitions:
            num_transitions = len(path.dfa.transitions[path.dfa.current_state])
            state_complexity = min(1.0, num_transitions / 10.0)  # Normalize
        
        # Path length normalized
        path_length = min(1.0, len(path) / max_hop)
        
        # Acceptance probability based on current state and remaining transitions
        acceptance_probability = self._estimate_acceptance_probability(path)
        
        return PartialMatchFeatures(
            completion_progress=completion_progress,
            match_quality=match_quality,
            state_complexity=state_complexity,
            path_length=path_length,
            acceptance_probability=acceptance_probability
        )
    
    def predict_combined_utility(self, node: str, path: 'Path', graph: nx.Graph, 
                                query_context: Dict = None, position_in_window: int = 0) -> float:
        """Predict utility combining both event importance and PM importance"""
        # Extract features for both event and partial match
        event_features = self.extract_event_features(node, graph, query_context, position_in_window)
        pm_features = self.extract_pm_features(path)
        
        # Calculate event utility
        event_vector = np.array([
            event_features.temporal_recency,
            event_features.semantic_relevance,
            event_features.structural_importance,
            event_features.frequency_score,
            event_features.downstream_impact
        ])
        event_utility = np.dot(self.event_weights, event_vector)
        
        # Calculate PM utility
        pm_vector = np.array([
            pm_features.completion_progress,
            pm_features.match_quality,
            pm_features.state_complexity,
            pm_features.path_length,
            pm_features.acceptance_probability
        ])
        pm_utility = np.dot(self.pm_weights, pm_vector)
        
        # Combine event and PM utilities
        combined_base_utility = (self.config.event_pm_weight * event_utility + 
                               (1 - self.config.event_pm_weight) * pm_utility)
        
        # Add probabilistic component
        probabilistic_factor = self.beta_dist.rvs()
        
        # Final combined utility
        final_utility = 0.7 * combined_base_utility + 0.3 * probabilistic_factor
        
        return max(0, min(1, final_utility))
    
    def _calculate_event_type_frequency(self, node: str, graph: nx.Graph) -> float:
        """Calculate frequency score based on node embedding similarity patterns"""
        if len(self.event_window) < 2:
            return 0.5
            
        node_embedding = graph.nodes[node]['embedding']
        similarities = [util.cos_sim(node_embedding, graph.nodes[n]['embedding']).item() 
                       for n in self.event_window[-10:] if n in graph.nodes]
        
        if not similarities:
            return 0.5
            
        return min(1.0, np.mean(similarities))
    
    def _calculate_state_distance_to_acceptance(self, path: 'Path') -> int:
        """Calculate minimum distance from current state to any accept state"""
        if not path.dfa.current_state or not path.dfa.accept_states:
            return float('inf')
            
        if path.dfa.current_state in path.dfa.accept_states:
            return 0
            
        # Simple BFS to find shortest path to accept state
        visited = set()
        queue = deque([(path.dfa.current_state, 0)])
        
        while queue:
            current_state, distance = queue.popleft()
            if current_state in visited:
                continue
            visited.add(current_state)
            
            if current_state in path.dfa.accept_states:
                return distance
                
            if current_state in path.dfa.transitions:
                for edge, next_state in path.dfa.transitions[current_state].items():
                    if next_state not in visited:
                        queue.append((next_state, distance + 1))
        
        return float('inf')
    
    def _estimate_acceptance_probability(self, path: 'Path') -> float:
        """Estimate probability that this partial match will be accepted"""
        if not path.dfa.current_state:
            return 0.1
            
        if path.dfa.current_state in path.dfa.accept_states:
            return 1.0
            
        # Factor in path score, completion progress, and remaining transitions
        base_prob = min(1.0, path.score)
        distance_factor = 1.0 / (1.0 + self._calculate_state_distance_to_acceptance(path))
        
        return base_prob * distance_factor
    
    def update_window(self, node: str):
        """Update sliding window with new event"""
        self.event_window.append(node)
        if len(self.event_window) > self.config.window_size:
            self.event_window.pop(0)
    
    def update_model(self, predicted_utility: float, actual_outcome: float):
        """Update model based on observed outcomes with separate event/PM weight adaptation"""
        self.historical_utilities.append((predicted_utility, actual_outcome))
        
        if len(self.historical_utilities) > 20:
            recent_errors = [abs(pred - actual) for pred, actual in self.historical_utilities[-20:]]
            avg_error = np.mean(recent_errors)
            
            # Adapt both event and PM weights
            if avg_error > 0.1:
                adjustment = self.config.adaptation_rate * (0.5 - avg_error)
                self.event_weights *= (1 + adjustment)
                self.pm_weights *= (1 + adjustment)
                
                # Normalize weights
                self.event_weights /= np.sum(self.event_weights)
                self.pm_weights /= np.sum(self.pm_weights)

class AdaptiveSheddingController:
    """Enhanced controller for adaptive shedding combining event and PM utilities"""
    
    def __init__(self, config: SheddingConfig):
        self.config = config
        self.utility_predictor = CombinedUtilityPredictor(config)
        self.current_threshold = config.utility_threshold
        self.processing_times = []
        self.shed_count = 0
        self.total_events = 0
        self.threshold_history = []  # Track threshold adaptations
        self.qor_metrics = []        # Track quality of results
        
    def should_shed_event_with_pm(self, node: str, path: 'Path', graph: nx.Graph, 
                                 query_context: Dict = None, current_time: float = None,
                                 position_in_window: int = 0) -> Tuple[bool, float]:
        """Enhanced shedding decision considering both event and PM importance"""
        self.total_events += 1
        
        # Update event window for temporal analysis
        self.utility_predictor.update_window(node)
        
        # Predict combined utility using both event and PM features
        predicted_utility = self.utility_predictor.predict_combined_utility(
            node, path, graph, query_context, position_in_window
        )
        
        # Enhanced threshold prediction based on system state
        adaptive_threshold = self._predict_utility_threshold(current_time, path)
        
        # Shedding decision
        should_shed = predicted_utility < adaptive_threshold
        
        if should_shed:
            self.shed_count += 1
        else:
            # Track QoR for accepted events/PMs
            self._update_qor_metrics(predicted_utility, path)
            
        # Adapt threshold based on recent performance
        if current_time:
            self._adapt_threshold_advanced(current_time, predicted_utility, should_shed)
            
        return should_shed, predicted_utility
    
    def _predict_utility_threshold(self, current_time: float = None, path: 'Path' = None) -> float:
        """Predict optimal utility threshold to maintain latency bound"""
        base_threshold = self.current_threshold
        
        # Adjust based on current processing time pressure  
        if current_time and current_time > self.config.latency_bound_ms * 0.8:
            # Increase threshold to shed more aggressively when approaching latency bound
            time_pressure = (current_time / self.config.latency_bound_ms) - 0.8
            base_threshold += time_pressure * 0.2
            
        # Adjust based on PM state complexity
        if path and hasattr(path, 'dfa') and path.dfa.current_state:
            pm_features = self.utility_predictor.extract_pm_features(path)
            if pm_features.completion_progress > 0.8:
                # Lower threshold for nearly complete PMs
                base_threshold *= 0.9
            elif pm_features.acceptance_probability < 0.3:
                # Higher threshold for low-probability PMs
                base_threshold *= 1.1
                
        return max(0.05, min(0.95, base_threshold))
    
    def _adapt_threshold_advanced(self, current_time: float, predicted_utility: float, was_shed: bool):
        """Advanced threshold adaptation considering QoR impact"""
        self.processing_times.append(current_time)
        self.threshold_history.append(self.current_threshold)
        
        if len(self.processing_times) < 15:
            return
            
        # Calculate recent performance metrics
        recent_times = self.processing_times[-15:]
        recent_thresholds = self.threshold_history[-15:]
        avg_processing_time = np.mean(recent_times)
        
        # QoR-aware adaptation
        if len(self.qor_metrics) > 5:
            recent_qor = np.mean(self.qor_metrics[-10:])
            qor_trend = self._calculate_qor_trend()
            
            # Balance latency and QoR
            if avg_processing_time > self.config.latency_bound_ms:
                # Need to shed more, but consider QoR impact
                if recent_qor > 0.7:  # Good QoR, can afford to shed more
                    self.current_threshold += self.config.adaptation_rate * 1.2
                else:  # Poor QoR, shed more conservatively
                    self.current_threshold += self.config.adaptation_rate * 0.8
            elif avg_processing_time < self.config.latency_bound_ms * 0.7:
                # Can afford to keep more events
                if qor_trend > 0:  # QoR improving
                    self.current_threshold -= self.config.adaptation_rate * 1.1
                else:  # QoR declining, be more conservative
                    self.current_threshold -= self.config.adaptation_rate * 0.9
        else:
            # Fallback to simple adaptation when insufficient QoR data
            if avg_processing_time > self.config.latency_bound_ms:
                self.current_threshold += self.config.adaptation_rate
            elif avg_processing_time < self.config.latency_bound_ms * 0.8:
                self.current_threshold -= self.config.adaptation_rate
                
        # Keep threshold in bounds
        self.current_threshold = max(0.05, min(0.95, self.current_threshold))
    
    def _update_qor_metrics(self, utility: float, path: 'Path'):
        """Update quality of results metrics"""
        # Combine utility with path-specific quality indicators
        if path:
            pm_features = self.utility_predictor.extract_pm_features(path)
            qor_score = 0.6 * utility + 0.4 * pm_features.match_quality
        else:
            qor_score = utility
            
        self.qor_metrics.append(qor_score)
        
        # Keep only recent QoR metrics
        if len(self.qor_metrics) > 50:
            self.qor_metrics.pop(0)
    
    def _calculate_qor_trend(self) -> float:
        """Calculate trend in quality of results"""
        if len(self.qor_metrics) < 10:
            return 0
            
        recent_half = self.qor_metrics[-5:]
        earlier_half = self.qor_metrics[-10:-5]
        
        return np.mean(recent_half) - np.mean(earlier_half)
        
    def should_shed_event(self, node: str, graph: nx.Graph, 
                         query_context: Dict = None, current_time: float = None) -> Tuple[bool, float]:
        """Backward compatibility method - delegates to enhanced version with dummy path"""
        dummy_path = Path(node)  # Create minimal path for compatibility
        return self.should_shed_event_with_pm(node, dummy_path, graph, query_context, current_time)
    
    def get_enhanced_statistics(self) -> Dict[str, Any]:
        """Get comprehensive shedding statistics including QoR metrics"""
        basic_stats = self.get_statistics()
        
        enhanced_stats = {
            **basic_stats,
            'avg_qor': np.mean(self.qor_metrics) if self.qor_metrics else 0,
            'qor_trend': self._calculate_qor_trend(),
            'threshold_stability': np.std(self.threshold_history[-20:]) if len(self.threshold_history) > 5 else 0,
            'adaptive_threshold_used': len(self.threshold_history) > 0
        }
        
        return enhanced_stats
    
    def get_statistics(self) -> Dict[str, Any]:
        """Get basic shedding statistics"""
        shedding_rate = self.shed_count / max(1, self.total_events)
        avg_processing_time = np.mean(self.processing_times) if self.processing_times else 0
        
        return {
            'total_events': self.total_events,
            'shed_count': self.shed_count,
            'shedding_rate': shedding_rate,
            'current_threshold': self.current_threshold,
            'avg_processing_time': avg_processing_time
        }

def utility_calculation(node):
    node_embedding = node['embedding']
    norm_embedding = node_embedding / np.linalg.norm(node_embedding)
    # Use a fixed reference vector for cosine similarity
    reference_vector = np.ones_like(node_embedding)
    reference_vector /= np.linalg.norm(reference_vector)
    # Calculate cosine similarity
    cosine_sim = 1 - cosine(norm_embedding, reference_vector)
    # Directly use the sum of normalized embedding as a basic utility indicator
    basic_score = np.sum(norm_embedding)
    # Calculate utility as a weighted sum
    utility = 0.7 * basic_score + 0.3 * cosine_sim
    return utility

def semantic_bfs_with_advanced_shedding(start_node, target_rule, graph: nx.Graph, 
                                       shedding_config: SheddingConfig = None, max_hop=3):
    '''
    Semantic Regular Path Query with Advanced CEP-style Shedding
    Combines event importance and partial match importance for optimal QoR
    '''
    if shedding_config is None:
        shedding_config = SheddingConfig()
    
    result_paths = []
    dfa = DFA()
    dfa.process_pattern(target_rule)
    
    # Initialize enhanced shedding controller
    shedding_controller = AdaptiveSheddingController(shedding_config)
    
    # Use priority queue with combined event-PM utilities
    queue = []
    start_time = time.time()
    
    # Create query context for semantic relevance
    query_context = {
        'target_rule': target_rule,
        'target_embedding': encoder.encode(' '.join(target_rule))
    }
    
    # Initial path
    initial_path = Path(start_node, dfa)
    heapq.heappush(queue, (0, 0, start_node, initial_path))
    
    counter = 0
    processed_nodes = 0
    position_in_window = 0
    
    while queue and processed_nodes < shedding_config.max_queue_size:
        current_time = (time.time() - start_time) * 1000
        
        # Get next item from priority queue
        _, _, current_node, current_path = heapq.heappop(queue)
        processed_nodes += 1
        position_in_window += 1
        
        # Check if path is accepted
        if current_path.is_accepted():
            result_paths.append(current_path)
            continue
            
        # Skip if path is invalid or exceeds max hop
        if not current_path.if_valid() or current_node not in graph or len(current_path) > max_hop:
            continue
            
        # Process neighbors with enhanced shedding
        neighbors = list(graph.neighbors(current_node))
        
        for neighbor in neighbors:
            # Skip if already visited
            if neighbor in current_path.visited_nodes:
                continue
                
            # Apply enhanced shedding algorithm considering both event and PM importance
            should_shed, predicted_utility = shedding_controller.should_shed_event_with_pm(
                neighbor, current_path, graph, query_context, current_time, position_in_window
            )
            
            if should_shed:
                continue  # Drop this event/path based on combined utility
                
            # Get relation info
            rel = graph[current_node][neighbor]
            rel_embedding = rel['embedding']
            
            # Check if relation is acceptable for current path
            if current_path.if_accept_rel_embedding(rel_embedding):
                new_path = current_path.add_new_tuple((current_node, rel, neighbor))
                
                # Calculate priority using combined utility (negative for min-heap)
                priority = -predicted_utility
                counter += 1
                
                heapq.heappush(queue, (priority, counter, neighbor, new_path))
                
        # Early termination if latency bound exceeded
        if current_time > shedding_config.latency_bound_ms:
            break
    
    # Print enhanced shedding statistics
    stats = shedding_controller.get_enhanced_statistics()
    print(f"Advanced Shedding Statistics: {stats}")
    
    return result_paths

def semantic_bfs_with_shedding(start_node, target_rule, graph: nx.Graph, 
                              shedding_config: SheddingConfig = None, max_hop=3):
    '''
    Semantic Regular Path Query with Basic Shedding (Backward Compatibility)
    '''
    # Delegate to advanced version for better results
    return semantic_bfs_with_advanced_shedding(start_node, target_rule, graph, shedding_config, max_hop)

def semantic_bfs(start_node, target_rule, graph: nx.Graph, threshold, max_hop=3):
    '''
    Semantic Regular Path Query (Original Implementation)
    '''
    result_paths = []
    dfa = DFA()
    dfa.process_pattern(target_rule)
    queue = deque([(start_node, Path(start_node, dfa))])
    max_len = 0
    while queue:
        # selectiivty shedding
        current_node, current_path = queue.popleft()
        # if accepted
        if current_path.is_accepted():
            result_paths.append(current_path)
        if not current_path.if_valid() or current_node not in graph or len(current_path) > max_hop:
            continue
        for neighbor in graph.neighbors(current_node):
            if neighbor in current_path.visited_nodes or utility_calculation(graph.nodes[neighbor]) < threshold:
                continue
            rel = graph[current_node][neighbor]
            rel_embedding  = rel['embedding']
            # current path considering if accepting current neighbor
            if current_path.if_accept_rel_embedding(rel_embedding):
                queue.append((neighbor, current_path.add_new_tuple((current_node, rel, neighbor))))
    return result_paths
